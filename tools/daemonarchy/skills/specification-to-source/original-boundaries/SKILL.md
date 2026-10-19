---
name: original-boundaries
description: Preserve original native implementation authority and exact dependency custody when reviewing retained source-unit contracts.
metadata:
  author: "Literate AI maintainers <literate-ai-maintainers@users.noreply.github.com>"
schema: "urn:literate-ai:schema:v1:specification-to-source-skill"
skill_id: "original-boundaries"
version: "1.0.0"
title: "Original native source boundaries"
stages: ["plan", "generate"]
dependencies: []
limitations:
  - "No source authority transfer, execution or package qualification is granted."
trust: "repository-reviewed"
---
# Original native source boundaries

Source stays in its upstream hierarchy and remains implementation authority.
Review contracts against exact original public interfaces, licenses and tests.
Do not regenerate, quarantine or replace original source. A candidate generated
in a separately authorized disposable workspace cannot acquire source authority
from this skill, workflow or a successful build.

Use canonical owners and locked dependencies for shared inputs. Preserve source,
support, bootstrap and native variant identities; missing ownership must refuse.
Exact native tests, independent acceptance and per-unit pkg provenance are
mandatory before qualification. Catalogue, helper and aggregate results retain
their actual scope. Follow the source-unit guide and active queue.
