//! Neuro-symbolic naming layer (roadmap §9 / pilier 7).
//!
//! ## Optional by design
//!
//! This layer is **opt-in and degrades to a no-op**: it calls the Anthropic API
//! only when `ANTHROPIC_API_KEY` is set (and `curl` is available). Without a key
//! it returns the decompiled C unchanged — so ARET is fully functional with no
//! LLM, and *capable* of LLM enrichment when one is connected (the user pays for
//! their own calls). No network crate is pulled in: it shells out to `curl`.
//!
//! ## Safe by construction
//!
//! The model only ever proposes **names and comments**. Applying them is a pure
//! textual rename of `sub_<addr>` / `vN` / `local_*` / `arg_*` identifiers plus a
//! leading comment — it cannot change program semantics, so verified output stays
//! verified. Proposed names that are not valid C identifiers, are C keywords,
//! collide, or clash with the runtime helpers are rejected; the rest are applied.
#![allow(dead_code)]

use std::collections::{HashMap, HashSet};
use std::io::Write as _;
use std::process::{Command, Stdio};

/// A naming suggestion for one function.
#[derive(Default, Debug, PartialEq)]
pub struct Naming {
    /// Proposed function name (replaces `sub_<addr>`).
    pub function: Option<String>,
    /// Per-symbol renames: original identifier (`v1`, `local_4`, `arg_8`) → name.
    pub names: HashMap<String, String>,
    /// A one-line description, emitted as a leading comment.
    pub comment: Option<String>,
}

/// Build the user prompt from the emitted C of one function. Asks for a compact,
/// line-based reply (no JSON) so parsing is trivial and robust.
pub fn build_prompt(emitted_c: &str) -> String {
    format!(
        "You are naming variables in decompiled C. Below is one function ARET \
recovered from a binary (semantics are correct; only the names are synthetic: \
`sub_<hex>` is the function, `vN` are SSA temporaries, `local_*`/`arg_*` are \
stack slots, `field_<hex>` are struct fields). Suggest meaningful names.\n\n\
Reply with ONLY these lines (omit any you can't improve), nothing else:\n\
  FUNCTION: <name>\n\
  <orig>: <name>      (one per identifier, e.g. `v3: length`)\n\
  COMMENT: <one sentence describing what the function does>\n\
Names must be valid C identifiers (snake_case), not C keywords.\n\n\
```c\n{}\n```",
        emitted_c
    )
}

/// Parse the model's line-based reply into a [`Naming`].
pub fn parse_reply(reply: &str) -> Naming {
    let mut n = Naming::default();
    for line in reply.lines() {
        let line = line.trim().trim_start_matches(['-', '*', ' ']);
        let Some((key, val)) = line.split_once(':') else { continue };
        let key = key.trim();
        let val = val.trim().trim_matches(['`', '"', '\'', ' ']);
        if val.is_empty() {
            continue;
        }
        match key {
            "FUNCTION" | "function" => n.function = Some(val.to_string()),
            "COMMENT" | "comment" => n.comment = Some(val.to_string()),
            _ => {
                // An identifier rename `orig: name`.
                if is_renamable_orig(key) {
                    n.names.insert(key.to_string(), val.to_string());
                }
            }
        }
    }
    n
}

/// The original identifiers we allow the model to rename.
fn is_renamable_orig(s: &str) -> bool {
    s.starts_with('v') && s[1..].chars().all(|c| c.is_ascii_digit()) && s.len() > 1
        || s.starts_with("local_")
        || s.starts_with("arg_")
        || s.starts_with("sub_")
}

/// A small set of C keywords / reserved identifiers a name must not collide with.
fn is_c_keyword(s: &str) -> bool {
    matches!(
        s,
        "auto" | "break" | "case" | "char" | "const" | "continue" | "default" | "do" | "double"
            | "else" | "enum" | "extern" | "float" | "for" | "goto" | "if" | "inline" | "int"
            | "long" | "register" | "restrict" | "return" | "short" | "signed" | "sizeof"
            | "static" | "struct" | "switch" | "typedef" | "union" | "unsigned" | "void"
            | "volatile" | "while" | "bool" | "true" | "false"
    )
}

/// Is `s` a valid, safe C identifier for a rename target?
fn valid_name(s: &str) -> bool {
    !s.is_empty()
        && s.len() <= 40
        && s.chars().next().is_some_and(|c| c.is_ascii_alphabetic() || c == '_')
        && s.chars().all(|c| c.is_ascii_alphanumeric() || c == '_')
        && !is_c_keyword(s)
        // Never shadow the runtime-helper namespace or a sub_/vN form.
        && !s.starts_with("__")
        && !(s.starts_with('v') && s[1..].chars().all(|c| c.is_ascii_digit()))
        && !s.starts_with("sub_")
}

/// Replace whole-identifier occurrences of `from` with `to` in `src` (a token is
/// delimited by non-identifier characters), leaving everything else intact.
fn replace_ident(src: &str, from: &str, to: &str) -> String {
    let bytes = src.as_bytes();
    let mut out = String::with_capacity(src.len());
    let mut i = 0;
    let is_word = |c: u8| c.is_ascii_alphanumeric() || c == b'_';
    while i < bytes.len() {
        if src[i..].starts_with(from) {
            let before_ok = i == 0 || !is_word(bytes[i - 1]);
            let end = i + from.len();
            let after_ok = end >= bytes.len() || !is_word(bytes[end]);
            if before_ok && after_ok {
                out.push_str(to);
                i = end;
                continue;
            }
        }
        // Advance one UTF-8 char.
        let ch = src[i..].chars().next().unwrap();
        out.push(ch);
        i += ch.len_utf8();
    }
    out
}

/// Does `name` already occur as a whole identifier token in `src`? Used to reject
/// a proposed name that would collide with an existing symbol (a callee, runtime
/// helper, struct field, or another variable), which a pure rename can't allow.
fn occurs_as_token(src: &str, name: &str) -> bool {
    let bytes = src.as_bytes();
    let is_word = |c: u8| c.is_ascii_alphanumeric() || c == b'_';
    let mut from = 0;
    while let Some(rel) = src[from..].find(name) {
        let i = from + rel;
        let end = i + name.len();
        let before_ok = i == 0 || !is_word(bytes[i - 1]);
        let after_ok = end >= bytes.len() || !is_word(bytes[end]);
        if before_ok && after_ok {
            return true;
        }
        from = i + 1;
    }
    false
}

/// Apply a [`Naming`] to the emitted C of a function whose synthetic name is
/// `sub_<entry:x>`. Rejects invalid/colliding/duplicate names so the result
/// always still compiles. Returns the rewritten C.
pub fn apply(emitted_c: &str, n: &Naming, entry: u64) -> String {
    let mut renames: Vec<(String, String)> = Vec::new();
    let mut used: HashSet<String> = HashSet::new();
    let self_name = format!("sub_{:x}", entry);

    // A name is acceptable only if it is a valid identifier AND does not already
    // occur anywhere in the unit (so it can't shadow a callee/helper/field).
    let acceptable = |name: &str, used: &HashSet<String>| {
        valid_name(name) && !used.contains(name) && !occurs_as_token(emitted_c, name)
    };

    // Function name first.
    if let Some(f) = &n.function {
        if acceptable(f, &used) {
            used.insert(f.clone());
            renames.push((self_name.clone(), f.clone()));
        }
    }
    // Then identifier renames, longest original first for deterministic output.
    let mut items: Vec<(&String, &String)> = n.names.iter().collect();
    items.sort_by(|a, b| b.0.len().cmp(&a.0.len()).then(a.0.cmp(b.0)));
    for (orig, name) in items {
        if orig == &self_name {
            continue;
        }
        if acceptable(name, &used) {
            used.insert(name.clone());
            renames.push((orig.clone(), name.clone()));
        }
    }

    let mut out = emitted_c.to_string();
    for (from, to) in &renames {
        out = replace_ident(&out, from, to);
    }
    // Prepend the description as a comment on the function it documents.
    if let Some(c) = &n.comment {
        let fname = n
            .function
            .as_deref()
            .filter(|f| valid_name(f))
            .map(str::to_string)
            .unwrap_or_else(|| format!("sub_{:x}", entry));
        let needle = format!("{}(", fname);
        // Insert before the function *definition* (the line that opens with `{`).
        if let Some(pos) = out
            .match_indices(&needle)
            .map(|(i, _)| i)
            .find(|&i| out[i..].lines().next().is_some_and(|l| l.trim_end().ends_with('{')))
        {
            let line_start = out[..pos].rfind('\n').map(|i| i + 1).unwrap_or(0);
            out.insert_str(line_start, &format!("/* {} */\n", c.replace("*/", "* /")));
        }
    }
    out
}

/// Call Claude via `curl` against the Anthropic Messages API. Reads
/// `ANTHROPIC_API_KEY` (required) and `ANTHROPIC_MODEL` (optional, defaults to a
/// cheap recent model). Returns the model's text reply. Any missing key, missing
/// `curl`, or transport error is an `Err` — the caller then leaves the C as-is.
pub fn call_claude(prompt: &str) -> anyhow::Result<String> {
    let key = std::env::var("ANTHROPIC_API_KEY")
        .map_err(|_| anyhow::anyhow!("ANTHROPIC_API_KEY not set (LLM layer disabled)"))?;
    let model =
        std::env::var("ANTHROPIC_MODEL").unwrap_or_else(|_| "claude-haiku-4-5-20251001".into());

    let body = format!(
        "{{\"model\":\"{}\",\"max_tokens\":1024,\"messages\":[{{\"role\":\"user\",\"content\":\"{}\"}}]}}",
        json_escape(&model),
        json_escape(prompt)
    );

    let mut child = Command::new("curl")
        .args([
            "-sS",
            "--max-time",
            "60",
            "https://api.anthropic.com/v1/messages",
            "-H",
            &format!("x-api-key: {}", key),
            "-H",
            "anthropic-version: 2023-06-01",
            "-H",
            "content-type: application/json",
            "--data-binary",
            "@-",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| anyhow::anyhow!("curl not available: {}", e))?;
    child.stdin.take().unwrap().write_all(body.as_bytes())?;
    let out = child.wait_with_output()?;
    if !out.status.success() {
        anyhow::bail!("curl failed: {}", String::from_utf8_lossy(&out.stderr));
    }
    let resp = String::from_utf8_lossy(&out.stdout);
    json_text_field(&resp).ok_or_else(|| anyhow::anyhow!("no text in API response: {}", resp))
}

/// End-to-end: enrich the emitted C with LLM-suggested names if an API is
/// configured, else return it unchanged. Never fails (LLM errors degrade to the
/// original output).
pub fn annotate(emitted_c: &str, entry: u64) -> String {
    match call_claude(&build_prompt(emitted_c)) {
        Ok(reply) => apply(emitted_c, &parse_reply(&reply), entry),
        Err(_) => emitted_c.to_string(),
    }
}

/// Minimal JSON string-escaping for embedding text in a request body.
fn json_escape(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 8);
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out
}

/// Extract the first `"text":"…"` string value from the Anthropic response JSON
/// (the assistant's reply lives in `content[].text`), unescaping JSON escapes.
fn json_text_field(json: &str) -> Option<String> {
    let key = "\"text\":";
    let start = json.find(key)? + key.len();
    let rest = json[start..].trim_start();
    let rest = rest.strip_prefix('"')?;
    let mut out = String::new();
    let mut chars = rest.chars();
    while let Some(c) = chars.next() {
        match c {
            '"' => return Some(out),
            '\\' => match chars.next()? {
                'n' => out.push('\n'),
                't' => out.push('\t'),
                'r' => out.push('\r'),
                '"' => out.push('"'),
                '\\' => out.push('\\'),
                '/' => out.push('/'),
                'u' => {
                    let hex: String = chars.by_ref().take(4).collect();
                    if let Some(cp) = u32::from_str_radix(&hex, 16).ok().and_then(char::from_u32) {
                        out.push(cp);
                    }
                }
                other => out.push(other),
            },
            c => out.push(c),
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_line_reply() {
        let n = parse_reply(
            "FUNCTION: parse_len\nv3: length\n local_4 : count \nbogus line\nCOMMENT: Parses a length.",
        );
        assert_eq!(n.function.as_deref(), Some("parse_len"));
        assert_eq!(n.names.get("v3").map(String::as_str), Some("length"));
        assert_eq!(n.names.get("local_4").map(String::as_str), Some("count"));
        assert_eq!(n.comment.as_deref(), Some("Parses a length."));
    }

    #[test]
    fn rename_whole_tokens_only() {
        // `v1` must not match inside `v12` or `xv1`.
        let src = "uint64_t v1 = v12 + v1;";
        assert_eq!(replace_ident(src, "v1", "len"), "uint64_t len = v12 + len;");
    }

    #[test]
    fn apply_rejects_bad_names_and_renames_good() {
        let c = "uint64_t sub_10(uint64_t v1) {\n    return v1 + 1;\n}\n";
        let mut names = HashMap::new();
        names.insert("v1".to_string(), "count".to_string());
        let n = Naming {
            function: Some("do_thing".into()),
            names,
            comment: Some("Adds one.".into()),
        };
        let out = apply(c, &n, 0x10);
        assert!(out.contains("do_thing"));
        assert!(out.contains("count"));
        assert!(out.contains("/* Adds one. */"));
        assert!(!out.contains("sub_10"));
        assert!(!out.contains(" v1"));
    }

    #[test]
    fn apply_rejects_keyword_and_collision() {
        let c = "uint64_t sub_0(uint64_t v1, uint64_t v2) { return v1 + v2; }";
        let mut names = HashMap::new();
        names.insert("v1".to_string(), "int".to_string()); // keyword → rejected
        names.insert("v2".to_string(), "x".to_string());
        let n = Naming { function: None, names, comment: None };
        let out = apply(c, &n, 0);
        assert!(out.contains("v1")); // unchanged (keyword rejected)
        assert!(out.contains(" x")); // renamed
    }

    #[test]
    fn apply_rejects_name_colliding_with_existing_symbol() {
        // `memcpy` already occurs as a callee → must not be used as a rename.
        let c = "uint64_t sub_0(uint64_t v1) { return memcpy(v1, v1, 8); }";
        let mut names = HashMap::new();
        names.insert("v1".to_string(), "memcpy".to_string()); // collides → rejected
        let n = Naming { function: None, names, comment: None };
        let out = apply(c, &n, 0);
        assert!(out.contains("memcpy(v1, v1, 8)")); // both unchanged
    }

    #[test]
    fn no_text_field_is_none() {
        assert!(json_text_field("{\"error\":\"nope\"}").is_none());
        assert_eq!(json_text_field("{\"text\":\"hi\\nthere\"}").as_deref(), Some("hi\nthere"));
    }
}
