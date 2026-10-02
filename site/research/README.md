# G shape research

The selected, denser Fuller Lattice and its live implementation are documented in
[g-implementation-study.md](g-implementation-study.md). Start there when
reviewing the October 2026 changes.

- [Selected Fuller hero](current/fuller-hero.png): current production appearance.
- [Chinese page](current/fuller-zh-desktop.png) and
  [example placeholders](current/fuller-zh-examples.png): the plain-language
  copy revision and reserved space for worked examples.
- [Local comparison](g-comparison.html): current Fuller and the earlier ribbon
  reference, available through the development server only.
- [Earlier comparison capture](current/comparison.png): the two forms before
  the Fuller density increase.
- [Imagegen concepts and prompts](concepts/prompts.md): the visual references
  used to agree on the two alternatives.
- [Original shape paper](hero-shape-study.pdf) and
  [variation paper](hero-g-variations.pdf): historical proposals and reasoning.
- [Original Chinese design brief](ui-brief.zh.md): the earlier design questions.

The original papers and brief describe older geometry, tip depth, and closed-loop
proposals. They are retained as research history and do not describe the current
implementation. Open the comparison through `npm run dev`, rather than directly
from disk. The Pages build includes only the Fuller landing page and fallback;
the ribbon and comparison are not published.
