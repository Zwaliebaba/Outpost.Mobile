export const meta = {
  name: 'game-design-review',
  description: 'Economy/Systems and Combat/Mechanics design specialists audit the tree and its design documents against a Homeworld or EVE Online scale goal, verify every finding against the code, and write Design/GameDesignReview.md',
  whenToUse: 'Run when the owner wants a game-design gap analysis: what is missing or should change in the economy, the combat, and the many-player synchronization to reach the stated goal. Pass args {commit, date} and optionally {outputPath, focus}.',
  phases: [
    { title: 'Map', detail: 'one scout inventories the tree and the design documents' },
    { title: 'Specialists', detail: 'four economy and four combat designers, each with one lens' },
    { title: 'Verify', detail: 'one refuter per specialist re-reads the code behind every finding' },
    { title: 'Leads', detail: 'a discipline lead per panel turns the surviving findings into a roadmap' },
    { title: 'Director', detail: 'the creative director merges both roadmaps and writes the review' },
    { title: 'Critic', detail: 'a completeness critic reads the written review against the tree' },
  ],
}

// ------------------------------------------------------------------------------------------------
// Inputs. Date.now() is not available inside a workflow, so the caller stamps the run.
// ------------------------------------------------------------------------------------------------
const commit = (args && args.commit) || 'HEAD'
const date = (args && args.date) || 'undated'
const outputPath = (args && args.outputPath) || 'Design/GameDesignReview.md'
const focus = (args && args.focus) || ''

// ------------------------------------------------------------------------------------------------
// Shared framing every agent gets. The goal is stated once so eight specialists argue against the
// same bar rather than eight private ones.
// ------------------------------------------------------------------------------------------------
const GOAL = `
THE GOAL this review measures against: Outpost: Frontier becoming the next Homeworld or the next
EVE Online. Read both benchmarks precisely rather than as slogans:
- Homeworld: fleet-grain command with tactical readability, formations that matter, ballistic
  combat where geometry decides, hull classes with sharp counters, a persistent fleet you carry
  forward, resource harvesting that paces a campaign, and a feel where every order answers in
  under a frame or two.
- EVE Online: one persistent shard, a player-driven economy where every item is built by players
  from mined and refined material, ISK faucets (bounties, missions, insurance) balanced by sinks
  (taxes, fees, blueprint research, destruction), destruction as the master sink that makes loss
  meaningful, markets and industry chains, standings and sovereignty, and a server that survives
  thousands in one fight (time dilation, tick-driven, no client authority).
When you make a recommendation say which benchmark it serves and why the other does not need it.
${focus ? 'THE OWNER ADDED THIS FOCUS: ' + focus : ''}
`

const REPO_GUIDE = `
THE REPOSITORY (read before you judge; cite file:line at commit ${commit}):
- README.md and AGENTS.md "What is actually here" say what is built and what is deliberately absent.
  A gap that AGENTS.md already declares still counts here -- this review judges completeness
  against the goal, not honesty of the docs -- but say that it is declared.
- Design/*.md are OPEN designs (Combat.md, Universe.md, CrossShard.md, GalaxyMap.md and their
  slices). Design/Archive/*.md are landed designs (Fleets, Stations, Hostiles, Collision,
  MmoScalabilityPlan, MmoScalabilityReview, and more). Design/Decisions/README.md indexes 59
  architecture decision records; read the ones your lens touches.
- GameLogic/ is the simulation: Universe.h/.cpp (orders, docking, ledger, combat, jumps),
  ShipState.h (Standing, FleetOrderKind, OrderState), HullSpec.h and DeviceSpec.h (the hull and
  device tables), SimTuning.h, Publisher.h/.cpp (per-subscriber replication), InterestSet,
  SpatialIndex, Formation, Movement, PathGrid/PathIslands, UniverseSnapshot.h (the wire format),
  GalaxyLayout, UniverseLayout, StartingUniverse.
- NeuronServer/ServerHost is the fixed-tick host; NeuronCore has Transport, QuicTransport,
  LoopbackTransport, Pcg32, FrameClock. NeuronClient is the D3D12 renderer and input; Outpost/ is
  the composition root, HUD, FleetSheet, AssemblyScreen, UniverseView, ViewTuning.h, Server.cfg.
- Tests/GameLogicTests name what is pinned: CombatTests, FleetTests, DockingTests, StationTests,
  ProtectorTests, PublisherTests, InterestTests, JumpTests, UniverseStateTests.
- Tools/UniverseGen writes Universe.sav; the galaxy is one seed (ADR 0055, 0058).
Use Grep and Read. Do not guess at what exists: a claim that something is missing must name the
place you looked and what you found there instead. Do not modify any file.
`

const FINDING_RULES = `
RULES FOR FINDINGS:
- Each finding is either kind "missing" (a system the goal needs that is not in the tree or any
  open design) or kind "change" (something present whose current shape will have to be reshaped
  to reach the goal). Do not report code defects, naming, or style: that is not this review.
- Priority: P0 = the goal is unreachable without it; P1 = needed before public players arrive;
  P2 = needed for the full benchmark experience but can follow.
- Evidence is a list of "path:line -- what is there" strings, at least one per finding, and for a
  "missing" finding at least one line proving the absence (the enum that lacks the value, the
  README sentence that declares it absent, the reserved DeviceId that has no row).
- The proposal is high level: what to add or change and what shape it should take given the
  tree's own decisions (fixed 60 Hz tick, deterministic replay, view-record snapshots, fleet-grain
  orders, shard-scoped identity, ledger asked for not broadcast). A proposal that would break a
  standing ADR must say so and name the ADR.
- dependsOn names other systems (yours or another panel's) that must exist first.
- Aim for the eight to twelve findings that matter, not everything you can think of.
`

const FINDINGS_SCHEMA = {
  type: 'object',
  properties: {
    specialist: { type: 'string' },
    present: {
      type: 'array',
      description: 'What the tree already has that your lens cares about, and how far it goes',
      items: {
        type: 'object',
        properties: {
          system: { type: 'string' },
          evidence: { type: 'array', items: { type: 'string' } },
          assessment: { type: 'string' },
        },
        required: ['system', 'evidence', 'assessment'],
      },
    },
    findings: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          id: { type: 'string' },
          title: { type: 'string' },
          kind: { type: 'string', enum: ['missing', 'change'] },
          priority: { type: 'string', enum: ['P0', 'P1', 'P2'] },
          summary: { type: 'string' },
          evidence: { type: 'array', items: { type: 'string' } },
          proposal: { type: 'string' },
          benchmark: { type: 'string', enum: ['Homeworld', 'EVE', 'both'] },
          dependsOn: { type: 'array', items: { type: 'string' } },
        },
        required: ['id', 'title', 'kind', 'priority', 'summary', 'evidence', 'proposal', 'benchmark', 'dependsOn'],
      },
    },
  },
  required: ['specialist', 'present', 'findings'],
}

const VERDICTS_SCHEMA = {
  type: 'object',
  properties: {
    verdicts: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          id: { type: 'string' },
          verdict: { type: 'string', enum: ['confirmed', 'reframed', 'refuted'] },
          reason: { type: 'string' },
          correctedSummary: { type: 'string' },
          correctedEvidence: { type: 'array', items: { type: 'string' } },
          correctedPriority: { type: 'string', enum: ['P0', 'P1', 'P2'] },
        },
        required: ['id', 'verdict', 'reason'],
      },
    },
    missedByThisSpecialist: {
      type: 'array',
      description: 'Gaps inside this lens the specialist did not report and the refuter thinks are real',
      items: { type: 'string' },
    },
  },
  required: ['verdicts', 'missedByThisSpecialist'],
}

const LEAD_SCHEMA = {
  type: 'object',
  properties: {
    discipline: { type: 'string' },
    verdict: { type: 'string', description: 'Two or three sentences: where this discipline stands against the goal' },
    principles: { type: 'array', items: { type: 'string' }, description: 'Design principles the roadmap must hold to, derived from the tree\'s own ADRs' },
    roadmap: {
      type: 'array',
      items: {
        type: 'object',
        properties: {
          title: { type: 'string' },
          kind: { type: 'string', enum: ['missing', 'change'] },
          priority: { type: 'string', enum: ['P0', 'P1', 'P2'] },
          benchmark: { type: 'string', enum: ['Homeworld', 'EVE', 'both'] },
          rationale: { type: 'string' },
          evidence: { type: 'array', items: { type: 'string' } },
          proposal: { type: 'string' },
          dependsOn: { type: 'array', items: { type: 'string' } },
          sourceFindings: { type: 'array', items: { type: 'string' } },
        },
        required: ['title', 'kind', 'priority', 'benchmark', 'rationale', 'evidence', 'proposal', 'dependsOn', 'sourceFindings'],
      },
    },
    risks: { type: 'array', items: { type: 'string' } },
    droppedFindings: { type: 'array', items: { type: 'string' }, description: 'Finding ids merged away or dropped, with a word on why' },
  },
  required: ['discipline', 'verdict', 'principles', 'roadmap', 'risks', 'droppedFindings'],
}

const DIRECTOR_SCHEMA = {
  type: 'object',
  properties: {
    outputPath: { type: 'string' },
    headline: { type: 'string' },
    itemCount: { type: 'integer' },
    p0: { type: 'array', items: { type: 'string' } },
  },
  required: ['outputPath', 'headline', 'itemCount', 'p0'],
}

const CRITIC_SCHEMA = {
  type: 'object',
  properties: {
    gaps: { type: 'array', items: { type: 'string' } },
    wrongClaims: { type: 'array', items: { type: 'string' }, description: 'Sentences in the review the code contradicts, with the file:line that contradicts them' },
    fixed: { type: 'boolean', description: 'true if the critic corrected the review file in place' },
  },
  required: ['gaps', 'wrongClaims', 'fixed'],
}

// ------------------------------------------------------------------------------------------------
// The panels. Each specialist has one lens and is told what the others cover so the eight do not
// converge on the same three obvious gaps.
// ------------------------------------------------------------------------------------------------
const PANELS = [
  {
    discipline: 'Economy & Systems',
    leadBrief: `You are the lead economy and systems designer. Your panel models currencies,
      resource generation loops, production chains and the sinks and faucets that keep an economy
      from inflating. Your roadmap must make the economy a closed loop: where value enters, where it
      is transformed, where it leaves, and what the player does at each step.`,
    specialists: [
      {
        key: 'currency-ledger',
        title: 'Currency & ledger designer',
        lens: `Currencies, wallets, ownership and the ledger. What is a unit of value in this game
          today (the station ledger of hulls, Universe::LedgerRequest/LedgerReply, fleet composition)
          and what would a currency, a wallet per player, a station inventory of items and a
          transaction log need to look like on this tree. Consider: how value is stated on the wire
          (ledger is asked for, ADR 0051), what the save must carry (ADR 0057, UniverseStateTests),
          determinism (integer arithmetic only, no float money), and what a player owns when their
          fleet dies. The other economy specialists cover resource faucets, sinks, and markets --
          leave those to them.`,
      },
      {
        key: 'resource-loops',
        title: 'Resource generation loop designer',
        lens: `Faucets and production: mining, harvesting, refining, manufacturing, blueprints,
          salvage and loot. Start from FleetOrderKind::Mine (reserved, refused), DeviceKind::MiningTool
          (no row), the Miner and Hauler hulls, ADR 0016 (bodies are presentation, so nothing can be
          mined today), the asteroid field, and the galaxy generator. Design the loop from a rock to
          a hull the player can launch, at fleet grain, inside a deterministic 60 Hz tick. The other
          economy specialists cover currency, sinks, and markets -- leave those to them.`,
      },
      {
        key: 'sinks-inflation',
        title: 'Economic sinks & inflation designer',
        lens: `Where value leaves the economy: destruction (a hull at zero shatters -- is anything
          lost from a ledger?), insurance, repair, upkeep, ammunition and fuel, taxes and fees, gate
          tolls, station services, blueprint research, decay. Inspect what happens to a ship, its
          fleet slot and its owner's ledger on DespawnCause::Destroyed and on docking, and whether
          any faucet exists that a sink must balance. Propose the sink structure and the metrics a
          live team would need (faucet/sink telemetry, money supply, velocity) and where those would
          be measured on this tree. The other economy specialists cover currency, faucets, and
          markets -- leave those to them.`,
      },
      {
        key: 'markets-industry',
        title: 'Markets, trade & player-driven industry designer',
        lens: `Player-to-player value exchange and the systems layer that makes an economy
          player-driven: markets and order books, contracts, hauling and trade routes between
          stations and systems (GalaxyLayout, gates, jumps ADR 0056, CrossShard.md), station
          ownership, sovereignty and standings (ADR 0039, Standing enum), factions as economic
          actors (Vanguard, Vandal), NPC price seeding and regional price differences. Judge what the
          galaxy's shape already gives you (one seed, relative neighbourhood graph gates, ADR 0055)
          and what is missing for trade to be a reason to move. The other economy specialists cover
          currency, faucets and sinks -- leave those to them.`,
      },
    ],
  },
  {
    discipline: 'Combat & Mechanics',
    leadBrief: `You are the lead combat and mechanics designer. Your panel tunes responsiveness,
      class roles, the ability framework, and smooth synchronization across high player counts.
      Your roadmap must make combat readable, decisive, and fair at scale, on a deterministic
      fixed-tick server that never trusts a client.`,
    specialists: [
      {
        key: 'responsiveness',
        title: 'Responsiveness & feel designer',
        lens: `The path from a tap to a visible answer: PointerTracker, the order message, the
          per-subscriber order budget in Publisher, the 60 Hz tick (ADR 0045), the reliable lane for
          orders (ADR 0029), snapshot interpolation in UniverseView, the fleet's cruise-at-slowest
          rule, formation solve and separation. Measure what you can from the code (ticks of latency
          from order receipt to first motion, how the client shows acknowledgment before the state
          changes, what happens under 100 ms RTT and 2 percent loss). Judge it against Homeworld's
          immediacy and EVE's server-tick truthfulness. The other combat specialists cover class
          balance, abilities, and scale synchronization -- leave those to them.`,
      },
      {
        key: 'class-roles',
        title: 'Class roles & hull balance designer',
        lens: `The hull and device tables as a role system: HullSpec.h (ten hulls, speed, turn,
          capsule, hull points, loadouts), DeviceSpec.h (five guns, range to skin, cooldown ticks,
          integer damage, traverse), MOUNT_ARC constants, and Combat.md 13's pacing targets and
          CombatTests. Ask whether the roles counter each other (tracking versus speed, range bands,
          alpha versus sustained, escorts protecting capitals), whether a Battleship and an
          Interceptor have a reason to exist in the same fight, and what a Homeworld-grade or
          EVE-grade class system needs that one hull number and one damage figure cannot carry:
          shields, armour, resistances, signature, module fitting. The other combat specialists
          cover feel, abilities, and scale synchronization -- leave those to them.`,
      },
      {
        key: 'abilities',
        title: 'Ability & device framework designer',
        lens: `The device model as an ability framework. Today a device is a gun that cycles on a
          cooldown, and a MiningTool is reserved. Judge whether this shape extends to: active
          abilities with targets and durations, electronic warfare, repair and logistics, tackle,
          propulsion modules, cloaking, capacitor or energy as the shared resource, fleet-level
          commands (Homeworld tactics: evasive, neutral, aggressive; formation orders), and how an
          ability is stated on the wire (fire events ride the datagram lane, ADR 0053) and replayed
          deterministically. Propose the framework: what a device row needs, what a mount needs, and
          what the order set (FleetOrderKind) needs to grow into. The other combat specialists cover
          feel, class balance, and scale synchronization -- leave those to them.`,
      },
      {
        key: 'scale-sync',
        title: 'Synchronization at high player count designer',
        lens: `What keeps a thousand players in one fight consistent and smooth: Publisher's
          per-subscriber interest sets, order budgets and despawn cursors (ADR 0027, 0030, 0031),
          the datagram and reliable lanes, view-record snapshots (ADR 0009), fire events per shot on
          the datagram lane (ADR 0053 -- what happens at 500 ships firing), the fixed 60 Hz tick
          under load (no time dilation), shard-scoped identity and cross-shard handoff (ADR 0047,
          CrossShard.md), the SpatialIndex broad phase, and the MmoScalabilityReview's open items.
          Judge against EVE's single-shard fight with time dilation and against a Homeworld-scale
          multiplayer skirmish. Propose what must change in the replication and tick model, and what
          the client must do (prediction, interpolation windows, priority accumulators) to stay
          smooth. The other combat specialists cover feel, class balance, and abilities -- leave
          those to them.`,
      },
    ],
  },
]

// ------------------------------------------------------------------------------------------------
// Phase 1: one scout maps the tree so each specialist starts from the same inventory instead of
// eight of them re-discovering the directory listing.
// ------------------------------------------------------------------------------------------------
phase('Map')
log(`Mapping the tree at ${commit}`)
const map = await agent(`You are the scout for a game-design review of this repository.
${REPO_GUIDE}
Produce a compact inventory (under 1500 words) that a specialist designer can start from:
1. Which gameplay systems exist, each in one line with its main file(s): command and orders, fleets,
   formations, movement and pathfinding, docking and the ledger, factions and standings, combat
   (mounts, devices, fire pass, damage, death), stations, hostiles and protectors, the galaxy and
   jumps, the save, the replication seam, the HUD screens.
2. The full enum values of FleetOrderKind, DeviceId, DeviceKind, HullId, Standing, DespawnCause,
   and the fields of ShipState and of the hull and device tables, quoted with file:line.
3. Every open design document and what it leaves open; every archived design that names a debt
   to a future design (search for "economy", "mining", "trade", "cargo", "credits", "shield",
   "ability", "module", "time dilation", "prediction", "market" across Design/ and the code).
4. The "Deliberately not here yet" lists from README.md and AGENTS.md, quoted.
Return the inventory as plain text. Cite file:line for every claim.`, { label: 'scout', effort: 'medium' })

// ------------------------------------------------------------------------------------------------
// Phases 2-4, pipelined per panel: each panel's specialists run in parallel, each specialist's
// findings go straight to its own refuter (no barrier across specialists), and the lead waits
// only for its own panel. The two panels never wait on each other until the director.
// ------------------------------------------------------------------------------------------------
const specialistPrompt = (panel, s) => `You are the ${s.title} on the ${panel.discipline} panel of a
game-design review. YOUR LENS: ${s.lens}
${GOAL}
${REPO_GUIDE}
THE SCOUT'S INVENTORY OF THE TREE (verify anything you rely on):
${map}
${FINDING_RULES}
Prefix every finding id with "${s.key}-" and number them. Set specialist to "${s.title}".`

const refuterPrompt = (panel, s, result) => `You are a skeptical senior engineer who knows this
repository, reviewing the findings of the ${s.title} (${panel.discipline} panel). Their lens was:
${s.lens}
${GOAL}
${REPO_GUIDE}
For EACH finding below, open the cited files and try to REFUTE it. A finding is refuted when: the
system already exists in the tree (name the file:line); an open design in Design/*.md already
specifies it in enough detail that it is scheduled work rather than a gap (name the section);
the evidence cited does not say what the finding claims; or the proposal would break a standing
ADR without saying so. A finding is reframed when the gap is real but the summary, priority or
evidence is wrong -- supply the corrected fields. Otherwise it is confirmed. Default to
"reframed" rather than "confirmed" when the evidence is thin. Then list gaps inside this lens the
specialist missed, if any, each with a file:line proving the absence.
THE FINDINGS:
${JSON.stringify(result.findings, null, 2)}`

const leadPrompt = (panel, reviewed) => `${panel.leadBrief}
${GOAL}
${REPO_GUIDE}
Your four specialists reported, and a refuter checked each one against the code. Below is each
specialist's report with the refuter's verdicts. Build the ${panel.discipline} roadmap:
- Drop refuted findings. Apply corrections from reframed ones. Merge duplicates across specialists.
- Fold in refuter-reported misses you agree with, after checking the file:line they cite.
- Order the roadmap by dependency, then by priority. Each item carries the file:line evidence
  that survived, a high-level proposal that respects the tree's ADRs, and the benchmark it serves.
- State the design principles the roadmap holds to, derived from the tree's own decisions (fixed
  tick, deterministic replay, fleet grain, view records, ledger asked for, no client authority).
- Name the risks: where the goal and the tree's decisions pull against each other.
Cap the roadmap at fifteen items and list what you dropped or merged in droppedFindings.
THE REPORTS:
${JSON.stringify(reviewed, null, 2)}`

const panelResults = await pipeline(
  PANELS,
  // Stage 1: the four specialists of this panel, in parallel, each piped to its own refuter.
  panel => parallel(panel.specialists.map(s => async () => {
    const result = await agent(specialistPrompt(panel, s), { label: `specialist:${s.key}`, phase: 'Specialists', schema: FINDINGS_SCHEMA })
    if (!result)
    {
      log(`specialist ${s.key} returned nothing and is dropped from the ${panel.discipline} roadmap`)
      return null
    }
    log(`${s.title}: ${result.findings.length} findings`)
    const verdicts = await agent(refuterPrompt(panel, s, result), { label: `verify:${s.key}`, phase: 'Verify', schema: VERDICTS_SCHEMA })
    if (!verdicts)
      log(`refuter for ${s.key} returned nothing; its findings pass to the lead unverified`)
    return { specialist: s.title, key: s.key, present: result.present, findings: result.findings, verdicts: verdicts ? verdicts.verdicts : [], unverified: !verdicts, refuterMisses: verdicts ? verdicts.missedByThisSpecialist : [] }
  })).then(r => r.filter(Boolean)),
  // Stage 2: the panel lead synthesizes its own panel only.
  (reviewed, panel) => {
    const refuted = reviewed.flatMap(r => r.verdicts.filter(v => v.verdict === 'refuted')).length
    const total = reviewed.reduce((n, r) => n + r.findings.length, 0)
    log(`${panel.discipline}: ${total} findings, ${refuted} refuted, ${reviewed.flatMap(r => r.refuterMisses).length} misses raised`)
    return agent(leadPrompt(panel, reviewed), { label: `lead:${panel.discipline}`, phase: 'Leads', schema: LEAD_SCHEMA, effort: 'high' })
  },
)

const leads = panelResults.filter(Boolean)
if (leads.length < PANELS.length)
  log(`only ${leads.length} of ${PANELS.length} panel leads returned a roadmap; the review is written from what came back`)

// ------------------------------------------------------------------------------------------------
// Phase 5: the director needs both roadmaps at once, which is the one barrier this workflow earns.
// ------------------------------------------------------------------------------------------------
phase('Director')
const director = await agent(`You are the creative director of Outpost: Frontier writing the game-design
review that answers one question: what is missing, and what must change, for this tree to become the
next Homeworld or the next EVE Online.
${GOAL}
${REPO_GUIDE}
Design/README.md defines a Review: a point-in-time audit of the tree against a stated goal, citing
file:line at a named commit, changing nothing by itself. Design/Archive/MmoScalabilityReview.md is
the house style -- read its opening, verdict table and first sections and match the register:
prose over bullets, concrete, no hedging, credit first for what is right.
Two discipline leads hand you their roadmaps below. WRITE THE REVIEW to ${outputPath} with the Write
tool, in Markdown, structured as:
1. Title, then a header paragraph: commit ${commit}, date ${date}, the goal, the method (eight
   specialists in two panels, each finding refuted or confirmed against the code, two leads, one
   director) and the tag legend: **[Missing]** and **[Change]**, with P0/P1/P2 and the benchmark.
2. "Verdict": one paragraph and a table with a row per discipline (Economy & Systems; Combat &
   Mechanics; and a third row, Cross-cutting, for what both need), each with "Ready for the goal?"
   and the short version.
3. "What already serves the goal": the decisions in the tree that a Homeworld or an EVE would
   have made, with evidence. Credit first.
4. "Economy & Systems" and 5. "Combat & Mechanics": each lead's verdict, principles, and roadmap
   items as subsections "### N. Title [Missing|Change] [P0|P1|P2] [Homeworld|EVE|both]" with
   evidence, proposal, and depends-on. Keep the leads' file:line citations verbatim; do not invent
   any.
6. "Cross-cutting": what both panels depend on (the same ledger, the same wire growth, the save
   format, telemetry, the client-side prediction question) and where the two panels' proposals
   conflict, resolved.
7. "The order of work": one dependency-ordered list across both disciplines, P0 first, each line
   naming which design document it wants (a new Design/<Topic>.md or an amendment to an open one).
8. "Risks": where the goal pulls against a standing ADR, named.
9. "Out of scope": what this review did not judge (rendering, audio, code quality, UX polish).
Before writing, spot-check five of the citations by opening the file at that line; if one is wrong,
fix or drop it and say so in the review's header. Return the path, a one-sentence headline, the
item count, and the P0 titles.
THE ROADMAPS:
${JSON.stringify(leads, null, 2)}`, { label: 'director', schema: DIRECTOR_SCHEMA, effort: 'high' })

if (!director)
{
  log('the director returned nothing; the lead roadmaps are returned raw')
  return { commit, date, leads }
}

// ------------------------------------------------------------------------------------------------
// Phase 6: a completeness critic reads the written review against the tree, not against the
// roadmaps, so it catches what every agent above agreed to overlook.
// ------------------------------------------------------------------------------------------------
phase('Critic')
const critic = await agent(`You are the completeness critic for a game-design review. Read
${director.outputPath} in full. Then, against the repository itself and the goal below, answer:
${GOAL}
${REPO_GUIDE}
1. wrongClaims: every sentence in the review the code contradicts. Open the cited file:line for at
   least twelve citations spread across the sections and check each says what the review says.
   Grep for anything the review says is absent.
2. gaps: what a Homeworld or an EVE designer would ask about that the review does not mention at
   all -- progression and skills, missions and PvE content, corporations and social structures,
   new-player experience, persistence of the player across sessions, anti-cheat, telemetry,
   moderation, live-ops and content cadence. Only list what is genuinely missing from the review;
   check with Grep before claiming absence in the tree.
3. If you found wrong claims, correct them in place with Edit (minimal edits: fix or remove the
   sentence, keep the structure) and set fixed to true. Add a short "What this review may still
   miss" section before "Out of scope" listing your gaps, each in one line. Do not touch any
   other file.`, { label: 'critic', schema: CRITIC_SCHEMA, effort: 'high' })

return {
  commit,
  date,
  review: director.outputPath,
  headline: director.headline,
  itemCount: director.itemCount,
  p0: director.p0,
  critic: critic || { gaps: [], wrongClaims: [], fixed: false, note: 'critic returned nothing' },
  panels: leads.map(l => ({ discipline: l.discipline, verdict: l.verdict, items: l.roadmap.length, dropped: l.droppedFindings })),
}
