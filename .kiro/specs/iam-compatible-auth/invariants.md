# IAM and STS invariants

**Written**: November 8, 2026, at Clark's request: "Look at the IAM and STS
documentation for invariants that must be obeyed and ensure there are unit
tests for all of them."

This is the catalogue of the rules AWS's IAM and STS documentation says
must hold. Each one has an id, its source page, and either the unit test
that checks it or the task that owns it. A task that owns invariants is not
done until each of them has a named unit test, and the task's PR updates
the row here to name it.

Status values:
- **Tested**: the named test exists and passes.
- **Task N**: the code is not built yet. Task N writes the code and the
  test. The parameter rules for those tasks are already built and tested in
  `iam_validation.hpp`, so the task only has to call them.
- **Not adopted**: deliberately not enforced, with the reason.

Test files are under `services/iam/tests/` unless a path says otherwise:
- `val` is `iam_validation_test.cpp`;
- `sm` is `iam_state_machine_test.cpp`;
- `adm` is `iam_admin_test.cpp`;
- `ev` is `iam_evaluator_test.cpp`;
- `az` is `iam_authorizer_test.cpp`;
- `qa` is `iam_query_api_test.cpp`, and `scn` the AWS CLI step of `docker/iam_node_scenario_test.cpp`;
- `pol` is `iam_policy_test.cpp` with its corpus, where `corpus:<file>`
  names a file under `corpus/`;
- `sigv4` is `tests/sigv4_verifier_test.cpp`.

## Gaps this review found and fixed

The first pass of this catalogue found five places where the built code
broke a documented rule. All five are fixed in the same PR:

1. **Role attachment quota.** AWS's default quota is 20 managed policies
   per role, but the state machine allowed only 10, the user and group
   quota. `iam_limits::attached_per_role` (20) is now separate.
2. **Tag key case on users and roles.** On users and roles, AWS treats
   `Department` and `department` as the same key: tagging one replaces the
   other. The state machine kept both. Other resource types stay
   case-sensitive, as on AWS.
3. **Tag quota at create.** Creating a policy, instance profile or OIDC
   provider with more than 50 tags was not refused. Only users and roles
   were checked.
4. **The path `//`.** AWS's pattern `(/)|(/[!-~]+/)` needs at least one
   character between the slashes, but `valid_path` accepted `//`.
5. **Account alias.** AWS refuses two hyphens in a row and a 12-digit
   alias, which would read as an account id. `valid_alias` accepted both.

## Names, paths and identifiers

Sources: [IAM quotas][quotas], [CreateUser][createuser],
[CreateRole][createrole], [CreateAccountAlias][alias].

| Id | Invariant | Status |
|---|---|---|
| INV-NAME-1 | User and role names are 1–64 characters | Tested: sm `entity_name_bounds_per_kind` |
| INV-NAME-2 | Group, policy and instance-profile names are 1–128 characters | Tested: sm `entity_name_bounds_per_kind` |
| INV-NAME-3 | Entity names use only `[A-Za-z0-9+=,.@_-]` | Tested: sm `names_paths_and_ids`, `entity_name_bounds_per_kind` |
| INV-NAME-4 | Inline policy names are 1–128 Basic Latin characters other than `\ / * ?` and white space | Tested: val `inline_policy_names`, qa `inline_policies_round_trip_for_users_groups_and_roles` |
| INV-NAME-5 | Names are unique case-insensitively within an account, for every entity kind | Tested: sm `users_are_case_insensitive`, `names_unique_case_insensitively_for_every_kind`, `versions` (policies) |
| INV-NAME-6 | Inline policy names are unique per entity, and putting an existing name replaces that policy | Tested: sm `inline_size_quota_is_a_total` |
| INV-PATH-1 | A path is `/`, or `/…/` of 3–512 printable ASCII characters | Tested: sm `names_paths_and_ids`, `path_bounds` |
| INV-ID-1 | Unique ids are their type's prefix (AIDA, AGPA, AROA, ANPA, AIPA) and base-32 characters, 20 in all, never repeated | Tested: sm `names_paths_and_ids` |
| INV-ACCT-1 | An account id is 12 digits | Tested: adm `create_account_validates_and_conflicts` |
| INV-ACCT-2 | An alias is 3–63 of `[a-z0-9-]`, with no leading, trailing or doubled hyphen, and is not a 12-digit number | Tested: val `account_aliases`, adm `create_account_validates_and_conflicts`, qa `inv_acct_2_aliases_are_validated_set_listed_and_deleted` |
| INV-ACCT-3 | Aliases are unique | Tested: sm `aliases_are_unique` |
| INV-ARN-1 | ARN parameters are 20–2048 characters | Tested: val `arn_lengths`, qa `policy_arn_and_permissions_boundary_reach_the_condition_keys` |

## Tags

Sources: [Tag][tag], [Tagging IAM resources][idtags], [IAM quotas][quotas].

| Id | Invariant | Status |
|---|---|---|
| INV-TAG-1 | At most 50 tags per resource, checked on create and on tag | Tested: val `tag_lists`, sm `tags`, `tag_quota_on_every_create`, qa `inv_tag_bad_tags_fail_the_whole_request` |
| INV-TAG-2 | Keys are 1–128 characters of letters, numbers, spaces and `_ . : / = + - @` | Tested: val `tag_keys_and_values`, qa `inv_tag_bad_tags_fail_the_whole_request` |
| INV-TAG-3 | Values are 0–256 characters from the same set | Tested: val `tag_keys_and_values`, qa `inv_tag_bad_tags_fail_the_whole_request` |
| INV-TAG-4 | Neither a key nor a value may begin with `aws:` | Tested: val `tag_keys_and_values`, qa `inv_tag_bad_tags_fail_the_whole_request` |
| INV-TAG-5 | Keys are case-insensitive on users, roles and STS sessions, keeping the latest spelling, and case-sensitive on every other resource | Tested: val `tag_lists`, sm `tag_key_case_by_resource_kind`, qa `inv_tag_bad_tags_fail_the_whole_request`, `a_policy_has_versions_and_is_deleted_only_when_unused` (policy keys) |
| INV-TAG-6 | One invalid tag fails the whole request, and nothing is created | Tested: val `tag_lists`, sm `tag_quota_on_every_create` (the check runs before any mutation), qa `inv_tag_bad_tags_fail_the_whole_request` |

## Quotas

Source: [IAM quotas][quotas]. Every quota is configurable upward, and all
replicas must use the same values.

| Id | Invariant | Status |
|---|---|---|
| INV-QUOTA-1 | Managed policies attached: 20 per role, 10 per user or group | Tested: sm `default_attachment_quotas_differ_by_kind`, `attachments_count_and_block_delete` |
| INV-QUOTA-2 | All other quotas default to AWS's values | Tested: sm `default_quotas_match_aws` |
| INV-QUOTA-3 | Managed policy size is at most 6144 characters, not counting white space | Tested: pol `corpus:parse/syntax_and_limits.json`, sm `policy_size_quota` |
| INV-QUOTA-4 | Inline policies total at most 2048 per user, 5120 per group and 10240 per role | Tested: sm `inline_size_quota_is_a_total` |
| INV-QUOTA-5 | A role trust policy is at most 2048 characters | Tested: sm `trust_policy_size_quota`, pol corpus |
| INV-QUOTA-6 | At most 2 access keys per user and 2 for the root | Tested: sm `access_keys` |
| INV-QUOTA-7 | A user is in at most 10 groups | Tested: sm `groups_per_user_quota` |
| INV-QUOTA-8 | An instance profile holds at most 1 role | Tested: sm `roles_and_instance_profiles` |
| INV-QUOTA-9 | Account-level counts (1500 customer managed policies, 300 groups, 1000 roles, 1000 instance profiles, 100 OIDC providers) | Not adopted: Kythira accounts are self-hosted. Requirement 4.4 lists only the per-entity quotas. Task 16's GetAccountSummary reports these values without enforcing them. |

## Managed policies and versions

Sources: [CreatePolicyVersion][cpv], [DeletePolicyVersion][dpv],
[DeletePolicy][dp].

| Id | Invariant | Status |
|---|---|---|
| INV-POL-1 | A PolicyDocument is 1–131072 characters of tab, LF, CR and U+0020–U+00FF | Tested: val `policy_document_characters`, qa `inv_pol_1_policy_documents_are_checked_before_parsing` |
| INV-POL-2 | At most 5 versions; a sixth create is LimitExceeded | Tested: sm `versions`, qa `policy_versions_page_in_version_order` |
| INV-POL-3 | The default version cannot be deleted | Tested: sm `versions` (DeleteConflict), qa `a_policy_has_versions_and_is_deleted_only_when_unused` |
| INV-POL-4 | Version ids are never reused | Tested: sm `versions` |
| INV-POL-5 | A policy can be deleted only when it is unattached and has no version but the default | Tested: sm `versions`, `attachments_count_and_block_delete`, qa `a_policy_has_versions_and_is_deleted_only_when_unused` |
| INV-POL-6 | VersionId matches `v[1-9][0-9]*(\.[A-Za-z0-9-]*)?` | Tested: val `policy_version_ids`, qa `inv_pol_6_version_ids_are_checked` |
| INV-POL-7 | AWS-managed policies cannot be changed or deleted | Tested: sm `aws_managed_policies_are_read_only`, qa `aws_managed_policies_are_listed_and_read_but_not_changed` |

## Deleting entities

| Id | Invariant | Status |
|---|---|---|
| INV-DEL-1 | A user with keys, group memberships, inline or attached policies cannot be deleted (DeleteConflict) | Tested: sm `delete_user_conflicts` |
| INV-DEL-2 | A group with members, inline or attached policies cannot be deleted | Tested: sm `delete_conflicts_for_groups_and_roles` |
| INV-DEL-3 | A role with inline or attached policies, or in an instance profile, cannot be deleted | Tested: sm `delete_conflicts_for_groups_and_roles`, `roles_and_instance_profiles`, `attachments_count_and_block_delete`, qa `a_role_with_policies_cannot_be_deleted_until_they_are_removed`, scn |
| INV-DEL-4 | An instance profile holding a role cannot be deleted | Tested: sm `roles_and_instance_profiles`, qa `adding_a_role_to_an_instance_profile_needs_pass_role_to_ec2` |

## Roles

Source: [CreateRole][createrole].

| Id | Invariant | Status |
|---|---|---|
| INV-ROLE-1 | MaxSessionDuration is 3600–43200, default 3600 | Tested: val `role_parameters`; CreateRole and UpdateRole call it: qa `inv_role_1_create_and_update_role_check_max_session_duration` |
| INV-ROLE-2 | A description is at most 1000 characters from AWS's set | Tested: val `role_parameters`; CreateRole, UpdateRole and UpdateRoleDescription call it: qa `inv_role_2_role_descriptions_are_checked` |
| INV-ROLE-3 | A trust policy must name a Principal and must not name Resource or NotResource | Tested: pol `corpus:parse/policy_types.json`, qa `trust_policies_are_parsed_as_trust_policies` |

## Access keys

Source: [CreateAccessKey][cak].

| Id | Invariant | Status |
|---|---|---|
| INV-KEY-1 | The secret is returned once, and never stored or logged in plaintext | Tested: sm `access_keys`, `property_replicas_and_restores_agree` (no plaintext in the log or a snapshot), adm `list_accounts_carries_no_secrets` |
| INV-KEY-2 | A new key is Active, and status is only Active or Inactive | Tested: sm `access_keys` |
| INV-KEY-3 | With no UserName, the key belongs to the signer, who can be the root | Tested: qa `inv_key_3_without_a_user_name_the_key_is_the_signers`, sm `access_keys` |
| INV-KEY-4 | An inactive or deleted key cannot authenticate | Tested: az `inv_key_4_unknown_inactive_deleted_and_temporary_keys_are_invalid_tokens`, scn `cluster_by_service_name` (AWS CLI v2) |

## OIDC providers

Source: [CreateOpenIDConnectProvider][oidc].

| Id | Invariant | Status |
|---|---|---|
| INV-OIDC-1 | The URL is 1–255 characters, starts `https://`, and has no query, fragment, port or user info | Tested: val `provider_urls`; CreateOpenIDConnectProvider calls it: qa `inv_oidc_1_provider_urls_are_checked` |
| INV-OIDC-2 | At most 100 client ids, each 1–255 characters | Tested: val `client_ids_and_thumbprints`, sm `oidc_providers`; CreateOpenIDConnectProvider and AddClientIDToOpenIDConnectProvider call it: qa `inv_oidc_2_client_ids_are_checked` |
| INV-OIDC-3 | Thumbprints are 40 hex digits | Tested: val `client_ids_and_thumbprints`; CreateOpenIDConnectProvider and UpdateOpenIDConnectProviderThumbprint call it: qa `inv_oidc_3_thumbprints_are_checked` |
| INV-OIDC-4 | At most 5 thumbprints | Tested: sm `oidc_thumbprint_quota` |
| INV-OIDC-5 | One provider per URL per account | Tested: sm `oidc_providers`, qa `an_oidc_provider_is_created_read_updated_and_deleted` |

## Policy grammar

Sources: [IAM JSON policy grammar][grammar] and the element reference
pages. All of these rows are tested by pol and its corpus (147 cases), in
`corpus:parse/elements.json`, `policy_types.json` and
`syntax_and_limits.json`.

| Id | Invariant |
|---|---|
| INV-GRAM-1 | Version is `2012-10-17` or `2008-10-17`. When it is absent it means 2008-10-17, and anything else is refused. |
| INV-GRAM-2 | Statement is required and non-empty, and is an object or an array of objects |
| INV-GRAM-3 | Effect is `Allow` or `Deny`, case-sensitively |
| INV-GRAM-4 | A statement has exactly one of Action and NotAction, and exactly one of Resource and NotResource (except in trust policies, which have neither) |
| INV-GRAM-5 | Principal and NotPrincipal appear only in resource and trust policies, where one of them is required. NotPrincipal is never allowed in a trust policy. |
| INV-GRAM-6 | Sids are unique within a policy, and alphanumeric in identity policies |
| INV-GRAM-7 | Element names are case-sensitive, unknown elements are refused, and so are duplicate keys at any depth |
| INV-GRAM-8 | Actions and condition keys carry a vendor prefix, and resources are ARNs or `*` |
| INV-GRAM-9 | Action matching is case-insensitive, and ARN matching is case-sensitive, with `*` and `?` able to cross `:` and `/` |

## Conditions and variables (task 11)

Source: [condition operators][condops], [policy variables][vars],
[multivalued keys][multi]. Built in `iam_conditions.hpp`; the tests are
pol corpus files under `corpus/condition/` and `corpus/parse/condition_operators.json`,
plus pol suite `iam_conditions`.

| Id | Invariant | Status |
|---|---|---|
| INV-COND-1 | Condition blocks, the keys within a block, and the operators are ANDed; a key's values are ORed | Tested: `corpus:condition/logic_and_sets.json`, `string.json` |
| INV-COND-2 | A negated operator with several values is true only when none match (NOR) | Tested: `corpus:condition/string.json` |
| INV-COND-3 | A missing key makes a positive operator false, and a negated or `…IfExists` operator true | Tested: `corpus:condition/logic_and_sets.json` (IfExists on every family) |
| INV-COND-4 | `ForAllValues` with an empty or missing set is true, and `ForAnyValue` is false | Tested: `corpus:condition/logic_and_sets.json` |
| INV-COND-5 | `Null` tests whether the key is present | Tested: `corpus:condition/other_families.json` |
| INV-COND-6 | Unknown operators, and `Null` with `IfExists` or a set prefix, are MalformedPolicyDocument | Tested: `corpus:parse/condition_operators.json`, pol `operator_table_resolves_every_name` |
| INV-COND-7 | Each family compares as its type: String (exact, IgnoreCase, Like), Numeric, Date (ISO 8601 and epoch, as instants), Bool, BinaryEquals (base64), IpAddress (IPv4 and IPv6 CIDR), Arn (segment by segment, case-sensitive); a value that cannot be read as the type matches nothing | Tested: `corpus:condition/string.json`, `numeric.json`, `date.json`, `other_families.json` |
| INV-COND-8 | Condition key names are case-insensitive; the tag part of `aws:RequestTag/` and `aws:ResourceTag/` keys is not | Tested: `corpus:condition/logic_and_sets.json` |
| INV-COND-9 | Global condition keys are present only when Kythira knows them, never invented | Tested: pol `global_condition_keys_present_only_when_known` |
| INV-VAR-1 | Variables are substituted only under Version 2012-10-17. Under 2008-10-17 the text `${…}` is literal. | Tested: `corpus:condition/variables.json` |
| INV-VAR-2 | `${*}`, `${?}` and `${$}` produce literal characters, and `${key, 'default'}` supplies a default | Tested: `corpus:condition/variables.json`, pol `pattern_substitution_escapes_literals` |
| INV-VAR-3 | A variable with no value matches nothing: a positive operator is false, a negated one true, and a resource pattern matches no resource | Tested: `corpus:condition/variables.json` |
| INV-VAR-4 | Variables are substituted only in Resource and in String, Bool and Arn values, and a substituted value is always literal (never a wildcard) | Tested: `corpus:condition/variables.json` |
| INV-VAR-5 | Only single-valued keys can be variables | Tested: `corpus:condition/variables.json` |

## Evaluation (task 12)

Source: [policy evaluation logic][evallogic], its subpage on how the
enforcement code decides, and its cross-account page. Each row has a named
case in `ev` and corpus cases under `corpus/evaluate/`.

| Id | Invariant | Status |
|---|---|---|
| INV-EVAL-1 | Default deny: with no allow, the request is denied | Tested: ev `inv_eval_1_default_deny`, `corpus:evaluate/order_and_deny.json` |
| INV-EVAL-2 | An explicit deny in any applicable policy wins over every allow | Tested: ev `inv_eval_2_explicit_deny_in_any_policy_wins`, `corpus:evaluate/order_and_deny.json` |
| INV-EVAL-3 | The order is the deny scan, then the RCP and SCP seam, then resource-based, identity-based, boundary and session policies | Tested: ev `inv_eval_3_deny_scan_comes_before_every_allow`, `corpus:evaluate/order_and_deny.json` |
| INV-EVAL-4 | In the same account, a resource-policy allow grants without an identity allow. Role trust policies and KMS key policies are the exception: they must allow the principal themselves, and an identity allow alone is not enough. A grant naming the account (its id or root ARN) delegates to identity policies, so it needs an identity allow too. | Tested: ev `inv_eval_4_resource_grant_alone_except_trust_and_key_policies`, `corpus:evaluate/resource_same_account.json` |
| INV-EVAL-5 | A boundary limits identity-policy allows only. A resource policy naming an IAM user ARN is not limited by it. | Tested: ev `inv_eval_5_boundary_limits_identity_allows_only`, `corpus:evaluate/resource_same_account.json`, `corpus:evaluate/boundaries_sessions.json` |
| INV-EVAL-6 | A resource-policy grant to a role ARN is limited by the session's boundary and session policy, but a grant to the session ARN is not | Tested: ev `inv_eval_6_role_arn_grant_limited_session_arn_grant_not`, `corpus:evaluate/resource_same_account.json` |
| INV-EVAL-7 | A grant to a federated-user ARN is not limited by the session policy | Tested: ev `inv_eval_7_federated_user_arn_grant_not_limited_by_session_policy`, `corpus:evaluate/resource_same_account.json` (STS side: task 26) |
| INV-EVAL-8 | When a session policy is present, an identity-based allow also needs a session-policy allow | Tested: ev `inv_eval_8_session_policy_must_also_allow`, `corpus:evaluate/boundaries_sessions.json` |
| INV-EVAL-9 | Cross-account access needs an allow on both sides | Tested: ev `inv_eval_9_cross_account_needs_both_sides`, `corpus:evaluate/cross_account.json` |
| INV-EVAL-10 | The root user is allowed unless an explicit deny or the SCP seam stops it | Tested: ev `inv_eval_10_root_user_allowed_unless_denied`, `corpus:evaluate/principals.json` |
| INV-EVAL-11 | NotAction, NotResource and NotPrincipal invert the match, and a Deny with NotPrincipal exempts the listed principals | Tested: ev `inv_eval_11_not_elements_invert_and_not_principal_exempts`, `corpus:evaluate/order_and_deny.json`, `corpus:evaluate/principals.json` |
| INV-EVAL-12 | `Principal: "*"` narrowed by `aws:PrincipalArn` is treated like a grant to that principal: neither the boundary nor the session policy limits it | Tested: ev `inv_eval_12_principal_star_narrowed_by_principal_arn_is_not_limited`, `corpus:evaluate/resource_same_account.json` |

## STS parameters

Sources: [AssumeRole][assumerole], [GetSessionToken][gst],
[GetFederationToken][gft], [AssumeRoleWithWebIdentity][arwwi]. The rules
are built in `iam_validation.hpp` and tested now. Each owning task calls
them from its handler, and its handler test shows one refusal per rule.

| Id | Invariant | Status |
|---|---|---|
| INV-STS-1 | RoleSessionName is 2–64 characters of `[\w+=,.@-]` | Tested: val `session_names`; handler: sts `inv_sts_1_role_session_names_are_checked` (AssumeRole, task 17) and `inv_sts_1_inv_web_1_and_inv_dur_4_parameters_are_checked` (AssumeRoleWithWebIdentity, task 19) |
| INV-STS-2 | The federated user Name is 2–32 characters of the same set | Tested: val `session_names` (task 26) |
| INV-STS-3 | ExternalId is 2–1224 characters of `[\w+=,.@:/-]` | Tested: val `external_ids`; handler: sts `inv_sts_3_external_ids_are_checked` (task 17) |
| INV-STS-4 | An MFA SerialNumber is 9–256 characters of `[\w+=/:,.@-]` | Tested: val `mfa_parameters`; handler: sts `inv_sts_4_and_5_mfa_parameters_are_checked_then_refused` (task 17) |
| INV-STS-5 | TokenCode is exactly 6 digits | Tested: val `mfa_parameters`; handler: sts `inv_sts_4_and_5_mfa_parameters_are_checked_then_refused` (task 17) |
| INV-STS-6 | SourceIdentity is 2–64 characters and never starts with `aws:` | Tested: val `source_identity_and_sizes`; handler: sts `inv_sts_6_source_identity_is_checked_and_kept_down_a_chain` (task 18) |
| INV-STS-7 | MinimumSessionTokenSize is 0–4096 | Tested: val `source_identity_and_sizes`; handler: sts `inv_sts_7_minimum_session_token_size_is_checked` (task 17; task 26 owes GetFederationToken) |
| INV-WEB-1 | A WebIdentityToken is 4–20000 characters | Tested: val `source_identity_and_sizes`; handler: sts `inv_sts_1_inv_web_1_and_inv_dur_4_parameters_are_checked` (task 19) |
| INV-DUR-1 | AssumeRole defaults to 3600 seconds, within 900–43200 | Tested: val `assume_role`; handler: sts `inv_dur_1_and_2_assume_role_durations` (task 17) |
| INV-DUR-2 | The duration may not exceed the role's MaxSessionDuration: the request is refused, not shortened | Tested: val `assume_role`; handler: sts `inv_dur_1_and_2_assume_role_durations` (task 17) |
| INV-DUR-3 | Role chaining is capped at 1 hour: a longer request fails | Tested: val `assume_role`; handler: sts `inv_dur_3_role_chaining_is_capped_at_an_hour` (task 18) |
| INV-DUR-4 | AssumeRoleWithWebIdentity follows the AssumeRole rules | Tested: val `web_identity`; handler: sts `inv_sts_1_inv_web_1_and_inv_dur_4_parameters_are_checked` (task 19) |
| INV-DUR-5 | GetSessionToken and GetFederationToken allow 900–129600 seconds, default 43200 | Tested: val `session_and_federation_tokens`; handler: sts `inv_dur_5_and_6_session_token_durations` (task 17; task 26 owes GetFederationToken) |
| INV-DUR-6 | For the root, those two default to 3600, and a longer request is shortened to 3600 | Tested: val `session_and_federation_tokens`; handler: sts `inv_dur_5_and_6_session_token_durations` (task 17; task 26 owes GetFederationToken) |
| INV-SPOL-1 | An inline session Policy is 1–2048 characters of the policy character set | Tested: val `session_policies`; handler: sts `inv_spol_1_to_4_session_policies_are_checked` (task 18) |
| INV-SPOL-2 | At most 10 PolicyArns | Tested: val `session_policies`; handler: sts `inv_spol_1_to_4_session_policies_are_checked` (task 18) |
| INV-SPOL-3 | PolicyArns name policies in the role's or user's own account, or AWS-managed ones | Tested: val `session_policies`; handler: sts `inv_spol_1_to_4_session_policies_are_checked` (task 18) |
| INV-SPOL-4 | The inline policy plus the ARNs total at most 2048 characters (PackedPolicyTooLarge) | Tested: val `session_policies`; handler: sts `inv_spol_1_to_4_session_policies_are_checked` (task 18) |
| INV-STAG-1 | At most 50 session tags, with valid keys and values, unique case-insensitively | Tested: val `session_tags`; handler: sts `inv_stag_1_to_3_session_tags_are_checked_and_pass_down_a_chain` (task 18) |
| INV-STAG-2 | At most 50 TransitiveTagKeys, each 1–128 characters | Tested: val `session_tags`; handler: sts `inv_stag_1_to_3_session_tags_are_checked_and_pass_down_a_chain` (task 18) |
| INV-STAG-3 | A tag that the calling session carries as transitive cannot be passed again | Tested: val `session_tags`; handler: sts `inv_stag_1_to_3_session_tags_are_checked_and_pass_down_a_chain` (task 18) |
| INV-STAG-4 | Session tags need `sts:TagSession` in the trust policy | Tested: sts `inv_stag_4_session_tags_need_tag_session_in_the_trust_policy` (task 18) |
| INV-STAG-5 | Session tags override the principal's tags of the same key, case-insensitively | Tested: val `session_tags_override_principal_tags`; sts `inv_stag_5_session_tags_override_the_roles_tags` (task 18) |

## STS behaviour and temporary credentials

| Id | Invariant | Status |
|---|---|---|
| INV-CRED-1 | GetSessionToken credentials may call only AssumeRole and GetCallerIdentity among the STS actions | Tested: val `sts_actions_by_credential` (wired by task 26) |
| INV-CRED-2 | GetFederationToken credentials may call only GetCallerIdentity | Tested: val `sts_actions_by_credential` (task 26) |
| INV-CRED-3 | Role credentials may not call GetFederationToken or GetSessionToken | Tested: val `sts_actions_by_credential` (task 26) |
| INV-CRED-4 | GetSessionToken and GetFederationToken need long-term credentials | Tested: val `sts_actions_by_credential` (task 26) |
| INV-CRED-5 | GetSessionToken credentials may call IAM only with MFA in the session | Tested: val `iam_api_by_credential` (task 26) |
| INV-CRED-6 | GetFederationToken credentials may never call IAM | Tested: val `iam_api_by_credential` (task 26) |
| INV-STS-B1 | AssumeRole needs the trust policy to allow the caller. Across accounts, the caller's identity policy must also allow `sts:AssumeRole`. | Tested: sts `inv_sts_b1_cross_account_assume_role_needs_both_sides`, `same_account_trust_naming_the_account_needs_an_identity_grant` (task 17) |
| INV-STS-B2 | When a trust policy sets `sts:ExternalId`, a missing or different ExternalId is denied | Tested: sts `inv_sts_b2_external_id_must_match` (task 17) |
| INV-STS-B3 | Credentials stop working at their Expiration (ExpiredToken) | Tested: sts `inv_sts_b3_credentials_stop_working_at_their_expiration` (task 17) |
| INV-STS-B4 | GetCallerIdentity needs no permission and is never denied | Tested: sts `inv_sts_b4_get_caller_identity_is_never_denied` (task 17) |
| INV-STS-B5 | GetFederationToken with no session policy gives a session that can do nothing. With one, the session gets the intersection of the user's policies and the session policy. | Task 26 |
| INV-STS-B6 | A web identity token is signed with RS256/384/512 or ES256/384/512, its `iss` matches a registered provider, its `aud` is one of that provider's client ids, and it is unexpired. Otherwise the call is refused with InvalidIdentityToken or ExpiredToken. | Tested: sts `inv_sts_b6_tokens_are_checked_before_the_trust_policy`, `an_unknown_kid_refetches_the_jwks_once` (task 19). The expiry error is AWS's `ExpiredTokenException`. |
| INV-STS-B7 | AssumeRoleWithWebIdentity is unsigned, and the role's trust policy must allow the provider's federated principal | Tested: sts `inv_sts_b7_an_unsigned_call_needs_the_provider_in_the_trust_policy`; container scenario step 4 (task 19) |
| INV-STS-B8 | The assumed-role ARN is `arn:<partition>:sts::<account>:assumed-role/<role>/<session>` | Tested: sts `inv_sts_b8_assume_role_returns_the_assumed_role_arn_and_credentials` (task 17) |
| INV-STS-Q1 | STS requests are limited to 600 per second per account | Not adopted: this is AWS's service quota, not a correctness rule. Kythira's request limits are its front end's. |

## SigV4

Source: [Signature Version 4][sigv4]. The verifier predates this catalogue,
and all of these rows are tested by sigv4.

| Id | Invariant | Test |
|---|---|---|
| INV-SIG-1 | The canonical request and string to sign match AWS's test suite | `aws_signing_test_suite_v4` |
| INV-SIG-2 | A header-signed request is within 15 minutes of the server clock | `clock_skew_window` |
| INV-SIG-3 | A presigned URL's X-Amz-Expires is 1–604800, and the URL fails after it | `presigned_expiry_and_bounds` |
| INV-SIG-4 | The credential scope's date, region and service match the request | `scope_is_checked` |
| INV-SIG-5 | `host` and every `x-amz-*` header must be signed | `host_and_amz_headers_must_be_signed` |
| INV-SIG-6 | Any change to a signed part breaks the signature | `tampering_breaks_the_signature` |

[quotas]: https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_iam-quotas.html
[createuser]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_CreateUser.html
[createrole]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_CreateRole.html
[alias]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_CreateAccountAlias.html
[tag]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_Tag.html
[idtags]: https://docs.aws.amazon.com/IAM/latest/UserGuide/id_tags.html
[cpv]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_CreatePolicyVersion.html
[dpv]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_DeletePolicyVersion.html
[dp]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_DeletePolicy.html
[cak]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_CreateAccessKey.html
[oidc]: https://docs.aws.amazon.com/IAM/latest/APIReference/API_CreateOpenIDConnectProvider.html
[grammar]: https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_grammar.html
[condops]: https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_elements_condition_operators.html
[vars]: https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_variables.html
[multi]: https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_condition-single-vs-multi-valued-context-keys.html
[evallogic]: https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_policies_evaluation-logic.html
[assumerole]: https://docs.aws.amazon.com/STS/latest/APIReference/API_AssumeRole.html
[gst]: https://docs.aws.amazon.com/STS/latest/APIReference/API_GetSessionToken.html
[gft]: https://docs.aws.amazon.com/STS/latest/APIReference/API_GetFederationToken.html
[arwwi]: https://docs.aws.amazon.com/STS/latest/APIReference/API_AssumeRoleWithWebIdentity.html
[sigv4]: https://docs.aws.amazon.com/IAM/latest/UserGuide/reference_sigv.html
