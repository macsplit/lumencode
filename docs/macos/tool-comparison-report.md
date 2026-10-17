# LumenCode Tool Comparison Report

This report compares LumenCode with three existing tools that are reasonably close in spirit.

The goal here is not “which product is best in general”.

The goal is:

- which tools feel most similar to what LumenCode is trying to become
- what features they already have
- what they still do better than LumenCode today
- what LumenCode already does differently

## How these three were chosen

I picked the tools that seem closest to LumenCode's real job:

- understanding an existing codebase
- navigating symbols and references
- exploring callers and callees
- using graphs or relationship views to reduce confusion

The three closest matches I found were:

1. `Understand`
2. `Sourcetrail`
3. `CodeQuery`

## Short take

### Understand

This is the strongest commercial benchmark.

It is the most mature all-round code understanding product in this group, especially for:

- cross references
- call trees
- dependency graphs
- metrics
- code compliance and static analysis

If we want a “serious incumbent” reference point, this is it.

### Sourcetrail

This is the clearest conceptual cousin to LumenCode.

It is much closer to the “interactive source explorer” idea:

- graph-first browsing
- symbol relationships
- offline exploration of unfamiliar code

Its main drawback is product status: the original upstream was archived in 2021, although community activity still exists around forks and related repos.

### CodeQuery

This is the leaner, more old-school comparison point.

It is less ambitious visually than Understand or Sourcetrail, but it does a lot of the classic code-browsing jobs well:

- symbol lookup
- caller/callee queries
- inheritance views
- file viewer
- graph export

It is a useful benchmark for “small but practical source explorer”.

## Feature comparison

| Feature | LumenCode | Understand | Sourcetrail | CodeQuery |
|---|---|---|---|---|
| macOS support | Yes | Yes | Yes | Yes |
| Linux support | Other Version | Yes | Yes | Yes |
| Works offline | Yes | Yes | Yes | Yes |
| GUI source explorer | Yes | Yes | Yes | Yes |
| CLI/internal harness | Yes, intentionally | API and automation focused | Not a major focus | Separate indexing and GUI workflow |
| Symbol list and inspector | Yes | Yes | Yes | Yes |
| Callers / callees | Partial, growing | Strong | Strong | Strong |
| Dependency / graph views | Partial, growing | Strong | Strong | Moderate |
| Lower code pane / snippet view | Yes | Yes | Yes | Yes |
| Open in external editor | Yes | Yes | Yes | Yes |
| Cross-language / cross-domain web linkage | Early and custom | Not the main pitch | Not the main pitch | Limited |
| Frontend-specific HTML/CSS/JS linking | Early and custom | Not highlighted | Not highlighted | Limited |
| Shared GUI + CLI architecture for closed-loop parser work | Yes | Not the main model | No | No |
| Current maintenance status | Active local project | Active commercial product | Original upstream archived; community activity exists | Active, smaller project |

## Practical feature notes

### Where Understand is ahead

Understand is clearly ahead on:

- mature call tree exploration
- control-flow and dependency graphs
- metrics
- compliance checks
- deep static analysis breadth
- large-team polish

LumenCode is not close to Understand yet on analysis depth or breadth.

### Where Sourcetrail is ahead

Sourcetrail is ahead on:

- polished graph-centric exploration
- mature symbol relationship navigation
- “I inherited this codebase, help me understand it” workflow

Conceptually, this is probably the nearest match to LumenCode's intended feel.

### Where CodeQuery is ahead

CodeQuery is ahead on:

- proven classic caller/callee lookup
- class inheritance querying
- practical lightweight code browsing
- graph export

It is simpler than the others, but it covers core source-explorer jobs cleanly.

## Where LumenCode is different

LumenCode is not just trying to clone a generic source browser.

The unusual things about it are:

- the GUI and CLI are deliberately paired so parser work can improve in a closed loop
- the architecture is split cleanly between app shell, shared Swift core, and native parser engine
- it is growing custom cross-domain linkage, especially for frontend projects
- it is being developed in parity with an existing Kirigami version rather than as a greenfield standalone product

That means LumenCode's most interesting angle is probably not “be another call graph tool”.

It is more like:

- a project understanding environment
- with code-explorer behavior
- plus custom relationship logic
- plus a developer harness for improving the backend quickly

## Best benchmark for each question

If the question is “what should the polished explorer feel like?”:

- `Sourcetrail`

If the question is “what is the most fully developed commercial competitor?”:

- `Understand`

If the question is “what is the smallest practical benchmark for caller/callee and symbol browsing?”:

- `CodeQuery`

## What this suggests for LumenCode

The best next comparisons to borrow from are:

1. From `Understand`
   - stronger call trees
   - richer type-aware symbol inspection
   - better project-wide relationship views

2. From `Sourcetrail`
   - smoother graph-driven navigation
   - clearer relationship-centered browsing
   - stronger onboarding flow for unfamiliar code

3. From `CodeQuery`
   - simple, reliable caller/callee workflows
   - graph export mindset
   - lightweight directness

## Bottom line

If I had to summarize the competitive picture in one sentence:

LumenCode is currently closest in spirit to `Sourcetrail`, closest in ambition gap to `Understand`, and closest in practical source-explorer baseline to `CodeQuery`.

## Sources

- [Understand official site](https://scitools.com/)
- [Sourcetrail original repository](https://github.com/CoatiSoftware/Sourcetrail)
- [OpenSourceSourceTrail GitHub organization](https://github.com/OpenSourceSourceTrail)
- [CodeQuery official site](https://ruben2020.github.io/codequery/)
