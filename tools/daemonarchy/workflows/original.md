---
schema: "literate-ai/generation-workflow-markdown@1"
workflow_id: "daemonarchy-source-review"
version: "1.0.0"
stages:
  - stage_id: "plan"
    kind: "model"
    dependencies: []
    response_schema_name: "sample_implementation_plan"
    content_kind: "metadata"
    required_capabilities:
      - "structured-output"
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "generate"
    kind: "model"
    dependencies:
      - "plan"
    response_schema_name: "sample_source_tree"
    content_kind: "source"
    required_capabilities:
      - "structured-output"
      - "source-generation"
    produces_tree: true
    maximum_output_tokens: null
  - stage_id: "validate"
    kind: "lifecycle"
    dependencies:
      - "generate"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "classify"
    kind: "lifecycle"
    dependencies:
      - "validate"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "authorize-build"
    kind: "lifecycle"
    dependencies:
      - "classify"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "build"
    kind: "lifecycle"
    dependencies:
      - "authorize-build"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "resolve-dependencies"
    kind: "lifecycle"
    dependencies:
      - "build"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "test-generated"
    kind: "lifecycle"
    dependencies:
      - "resolve-dependencies"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "verify-independent"
    kind: "lifecycle"
    dependencies:
      - "test-generated"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "prepare-tree"
    kind: "lifecycle"
    dependencies:
      - "verify-independent"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
  - stage_id: "commit-tree"
    kind: "lifecycle"
    dependencies:
      - "prepare-tree"
    response_schema_name: ""
    content_kind: "metadata"
    required_capabilities: []
    produces_tree: false
    maximum_output_tokens: null
---
# Retained contract and candidate-only workflow

This declaration satisfies the framework's generation-workflow schema. It does
not qualify generation, native execution or source-authority transfer. The
current routing policy is metadata-only and no lifecycle driver is configured.
Existing native commands remain in tools/daemonarchy/README.md; source-unit acceptance is separately declared.

## Stage: plan

Review the selected requirements and propose a separate candidate plan. Retain
existing implementation authority, native platform boundaries and unresolved
acceptance. Do not infer source regeneration or release authorization.

## Stage: generate

Only after a separately acknowledged implementation task and qualified execution
path, produce a disposable candidate tree from approved specifications. Never
write over retained source or treat the candidate as accepted implementation.
A typed native adapter, exact inputs and independent acceptance are required
before a candidate can be admitted. This adoption does not execute this stage.
