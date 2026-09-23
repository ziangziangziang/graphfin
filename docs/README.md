# GraphFin documentation

GraphFin is a general-purpose time-series graph database derived from TuGraph.
Many graph problems are also time-series problems: the database stores what is
connected and what changed along those connections, together. Investment
analytics is the first major use case; financial dependency analysis and
equipment telemetry are the acceptance fixtures.

Start with [the English README](../README.md) or [中文说明](../README_CN.md).

| Need | Read |
| --- | --- |
| Understand what is implemented | [Product and capability boundaries](product.md) |
| Build and run a local instance | [Developer quick start](getting-started.md) |
| Use series through clients | [Series client contracts](architecture/09-series-client-contracts.md) |
| Understand the combined architecture | [Architecture index](architecture/README.md) |
| Validate the merge | [Post-merge test plan](testing/post-merge.md) |
| Prepare a release | [Release process](../RELEASE.md) |
| Understand remaining work | [Combined roadmap](roadmap.md) |

The `en-US/source` and `zh-CN/source` trees are inherited TuGraph reference
manuals. Their examples, version numbers, hosting links and feature claims are
not a GraphFin release support matrix. The product pages above describe this fork.
Historical engineering reports retain their original commit/date scope.
