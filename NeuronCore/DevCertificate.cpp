#include "pch.h"
#include "DevCertificate.h"

#include <wincrypt.h>
#include <ncrypt.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "ncrypt.lib")

namespace Neuron
{
namespace
{
// The name the private key is persisted under, and the string in the removal command at the top of
// DevCertificate.h. Fixed rather than generated so that a second boot reuses the first boot's key
// instead of leaving a new one behind every time the game starts.
constexpr const wchar_t* DEV_KEY_NAME = L"Outpost.Dev.Quic";
constexpr const wchar_t* DEV_SUBJECT_NAME = L"CN=Outpost Development";
constexpr DWORD DEV_KEY_BITS = 2048;
constexpr std::int64_t DEV_VALIDITY_HUNDRED_NS = 365LL * 24LL * 60LL * 60LL * 10000000LL;

// NCrypt handles are ULONG_PTRs and do not close with CloseHandle, so NeuronHelper.h's ScopedHandle
// does not fit them. Two of them are live across a dozen fallible calls in Acquire, every one of
// which returns a diagnostic rather than throwing, so they close here instead of at each return.
//
// One NCRYPT_HANDLE serves for both the provider and the key because the SDK spells every one of
// its handle types as ULONG_PTR: they are the same type, not merely the same width. If that ever
// stops being true this stops compiling, which is the failure worth having -- a cast here would go
// on being accepted instead.
struct ScopedNCryptHandle
{
  NCRYPT_HANDLE handle = 0;

  ScopedNCryptHandle() = default;
  ~ScopedNCryptHandle()
  {
    if (handle != 0)
      NCryptFreeObject(handle);
  }

  ScopedNCryptHandle(const ScopedNCryptHandle&) = delete;
  ScopedNCryptHandle& operator=(const ScopedNCryptHandle&) = delete;
};

// Sets the properties a freshly created key needs and finalizes it. Returns the name of the call
// that failed, or nullptr on success, with the status in _outStatus. Split out so that the caller
// has one place to delete a half-made key from.
[[nodiscard]] const char* FinalizeNewKey(NCRYPT_HANDLE _key, SECURITY_STATUS& _outStatus) noexcept
{
  DWORD keyBits = DEV_KEY_BITS;
  _outStatus = NCryptSetProperty(_key, NCRYPT_LENGTH_PROPERTY, reinterpret_cast<PBYTE>(&keyBits), sizeof(keyBits), 0);
  if (FAILED(_outStatus))
    return "NCryptSetProperty NCRYPT_LENGTH_PROPERTY";

  // Signing only. TLS 1.3 never does RSA key exchange, so a signing key is the whole of what a QUIC
  // server needs -- and the key stays on the machine that made it, so it is not marked exportable
  // either, which is where this differs from MsQuic's own test-only version of the same recipe.
  DWORD keyUsage = NCRYPT_ALLOW_SIGNING_FLAG;
  _outStatus = NCryptSetProperty(_key, NCRYPT_KEY_USAGE_PROPERTY, reinterpret_cast<PBYTE>(&keyUsage), sizeof(keyUsage), 0);
  if (FAILED(_outStatus))
    return "NCryptSetProperty NCRYPT_KEY_USAGE_PROPERTY";

  _outStatus = NCryptFinalizeKey(_key, 0);
  if (FAILED(_outStatus))
    return "NCryptFinalizeKey";

  return nullptr;
}

// CryptEncodeObject is a two-call API: once for the size, once for the bytes. The result has to
// outlive CertCreateSelfSignCertificate, which is why it lands in a vector the caller holds.
[[nodiscard]] bool EncodeObject(LPCSTR _structType, const void* _structInfo, std::vector<std::uint8_t>& _outBytes)
{
  DWORD size = 0;
  if (!CryptEncodeObject(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, _structType, _structInfo, nullptr, &size))
    return false;

  _outBytes.assign(size, 0u);
  if (!CryptEncodeObject(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, _structType, _structInfo, _outBytes.data(), &size))
    return false;

  _outBytes.resize(size);
  return true;
}
} // namespace

DevCertificate::~DevCertificate()
{
  Release();
}

bool DevCertificate::Acquire()
{
  Release();

  ScopedNCryptHandle provider;
  SECURITY_STATUS status = NCryptOpenStorageProvider(&provider.handle, MS_KEY_STORAGE_PROVIDER, 0);
  if (FAILED(status))
  {
    SetReason("NCryptOpenStorageProvider failed (0x%08x)", static_cast<unsigned>(status));
    return false;
  }

  // Open the key this machine already has, and generate one only if it has none. NTE_BAD_KEYSET is
  // "no such key", which is the ordinary first-run answer and not a failure.
  ScopedNCryptHandle key;
  status = NCryptOpenKey(provider.handle, &key.handle, DEV_KEY_NAME, 0, NCRYPT_SILENT_FLAG);
  if (status == NTE_BAD_KEYSET)
  {
    status = NCryptCreatePersistedKey(provider.handle, &key.handle, NCRYPT_RSA_ALGORITHM, DEV_KEY_NAME, 0, 0);
    if (status == NTE_EXISTS)
    {
      // Somebody created it between our open and our create: another process, or -- the case this
      // tree actually reaches -- a second test worker on a machine seeing its first run. Their key
      // is as good as ours would have been, so take theirs rather than fail.
      status = NCryptOpenKey(provider.handle, &key.handle, DEV_KEY_NAME, 0, NCRYPT_SILENT_FLAG);
      if (FAILED(status))
      {
        SetReason("NCryptOpenKey failed on a key another process had just created (0x%08x)", static_cast<unsigned>(status));
        return false;
      }
    }
    else if (FAILED(status))
    {
      SetReason("NCryptCreatePersistedKey failed (0x%08x) -- the key store will not take a new key", static_cast<unsigned>(status));
      return false;
    }
    else if (const char* const failed = FinalizeNewKey(key.handle, status); failed != nullptr)
    {
      // A key created and not finalized stays in the store as something no later run can use, and
      // the next boot would find it and fail on it forever. It goes back out before we report.
      (void)NCryptDeleteKey(key.handle, NCRYPT_SILENT_FLAG);
      key.handle = 0; // NCryptDeleteKey frees the handle as well as the key
      SetReason("%s failed (0x%08x)", failed, static_cast<unsigned>(status));
      return false;
    }
    else
    {
      m_keyWasCreated = true;
    }
  }
  else if (FAILED(status))
  {
    SetReason("NCryptOpenKey failed (0x%08x) -- try: certutil -delkey -user Outpost.Dev.Quic", static_cast<unsigned>(status));
    return false;
  }

  // The subject name, as an ASN.1 blob. Two calls again: size, then bytes.
  DWORD subjectBytes = 0;
  if (!CertStrToNameW(X509_ASN_ENCODING, DEV_SUBJECT_NAME, CERT_X500_NAME_STR, nullptr, nullptr, &subjectBytes, nullptr))
  {
    SetReason("CertStrToNameW failed to size the subject name (0x%08x)", static_cast<unsigned>(GetLastError()));
    return false;
  }
  std::vector<std::uint8_t> subject(subjectBytes, 0u);
  if (!CertStrToNameW(X509_ASN_ENCODING, DEV_SUBJECT_NAME, CERT_X500_NAME_STR, nullptr, subject.data(), &subjectBytes, nullptr))
  {
    SetReason("CertStrToNameW failed to encode the subject name (0x%08x)", static_cast<unsigned>(GetLastError()));
    return false;
  }
  CERT_NAME_BLOB subjectBlob{subjectBytes, subject.data()};

  // Two extensions, both of which Schannel looks for in a server certificate: this key is for server
  // authentication, and it is a signing key. A subject alternative name is deliberately absent --
  // nothing checks it, because the client is configured not to validate at all (ADR 0023), and a SAN
  // that is never read is a claim nobody has tested.
  LPSTR serverAuth[1] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
  CERT_ENHKEY_USAGE enhancedUsage{};
  enhancedUsage.cUsageIdentifier = 1;
  enhancedUsage.rgpszUsageIdentifier = serverAuth;

  BYTE usageBits = CERT_DIGITAL_SIGNATURE_KEY_USAGE;
  CRYPT_BIT_BLOB usageBlob{};
  usageBlob.cbData = sizeof(usageBits);
  usageBlob.pbData = &usageBits;
  usageBlob.cUnusedBits = 0;

  std::vector<std::uint8_t> encodedEnhancedUsage;
  std::vector<std::uint8_t> encodedUsage;
  if (!EncodeObject(X509_ENHANCED_KEY_USAGE, &enhancedUsage, encodedEnhancedUsage) ||
      !EncodeObject(X509_KEY_USAGE, &usageBlob, encodedUsage))
  {
    SetReason("CryptEncodeObject failed on a certificate extension (0x%08x)", static_cast<unsigned>(GetLastError()));
    return false;
  }

  CERT_EXTENSION extensionRows[2]{};
  extensionRows[0].pszObjId = const_cast<LPSTR>(szOID_ENHANCED_KEY_USAGE);
  extensionRows[0].fCritical = FALSE;
  extensionRows[0].Value.cbData = static_cast<DWORD>(encodedEnhancedUsage.size());
  extensionRows[0].Value.pbData = encodedEnhancedUsage.data();
  extensionRows[1].pszObjId = const_cast<LPSTR>(szOID_KEY_USAGE);
  extensionRows[1].fCritical = FALSE;
  extensionRows[1].Value.cbData = static_cast<DWORD>(encodedUsage.size());
  extensionRows[1].Value.pbData = encodedUsage.data();

  CERT_EXTENSIONS extensions{};
  extensions.cExtension = static_cast<DWORD>(std::size(extensionRows));
  extensions.rgExtension = extensionRows;

  SYSTEMTIME startTime{};
  GetSystemTime(&startTime);
  FILETIME startFileTime{};
  if (!SystemTimeToFileTime(&startTime, &startFileTime))
  {
    SetReason("SystemTimeToFileTime failed (0x%08x)", static_cast<unsigned>(GetLastError()));
    return false;
  }
  ULARGE_INTEGER expiry{};
  expiry.LowPart = startFileTime.dwLowDateTime;
  expiry.HighPart = startFileTime.dwHighDateTime;
  expiry.QuadPart += static_cast<std::uint64_t>(DEV_VALIDITY_HUNDRED_NS);
  const FILETIME expiryFileTime{expiry.LowPart, expiry.HighPart};
  SYSTEMTIME expiryTime{};
  if (!FileTimeToSystemTime(&expiryFileTime, &expiryTime))
  {
    SetReason("FileTimeToSystemTime failed (0x%08x)", static_cast<unsigned>(GetLastError()));
    return false;
  }

  // The provider info is what lets Schannel find the private half later: the certificate itself
  // carries no key, only the name of the container this key lives in.
  CRYPT_KEY_PROV_INFO keyProvInfo{};
  keyProvInfo.pwszContainerName = const_cast<LPWSTR>(DEV_KEY_NAME);
  keyProvInfo.pwszProvName = const_cast<LPWSTR>(MS_KEY_STORAGE_PROVIDER);
  keyProvInfo.dwProvType = 0; // 0 means CNG rather than a legacy CryptoAPI provider type
  keyProvInfo.dwFlags = NCRYPT_SILENT_FLAG;
  keyProvInfo.dwKeySpec = AT_KEYEXCHANGE;

  CRYPT_ALGORITHM_IDENTIFIER signatureAlgorithm{};
  signatureAlgorithm.pszObjId = const_cast<LPSTR>(szOID_RSA_SHA256RSA);

  m_context =
    CertCreateSelfSignCertificate(key.handle, &subjectBlob, 0, &keyProvInfo, &signatureAlgorithm, &startTime, &expiryTime, &extensions);
  if (m_context == nullptr)
  {
    SetReason("CertCreateSelfSignCertificate failed (0x%08x)", static_cast<unsigned>(GetLastError()));
    return false;
  }

  m_reason[0] = '\0';
  return true;
}

void DevCertificate::Release() noexcept
{
  if (m_context != nullptr)
  {
    CertFreeCertificateContext(static_cast<PCCERT_CONTEXT>(m_context));
    m_context = nullptr;
  }
  m_keyWasCreated = false;
}

const void* DevCertificate::Context() const noexcept
{
  return m_context;
}

const char* DevCertificate::Reason() const noexcept
{
  return m_reason;
}

bool DevCertificate::KeyWasCreated() const noexcept
{
  return m_keyWasCreated;
}

void DevCertificate::SetReason(const char* _format, ...) noexcept
{
  std::va_list arguments;
  va_start(arguments, _format);
  (void)std::vsnprintf(m_reason, sizeof(m_reason), _format, arguments);
  va_end(arguments);
}
} // namespace Neuron
