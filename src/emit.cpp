// emit.cpp — code generation.
//   terse    canonical, compact Terse text (default)
//   json     structured object for programs and APIs
//   english  plain-English expansion, for models or teammates that don't know Terse
//   xml      XML-style tags (<task>, <context>, <rules>...), the layout Claude reads best
//   markdown bold labels and lists, for chat UIs and docs
//   yaml     same structure as json, for config files
#include "tersec.hpp"

#include <cstdio>

namespace terse {

const char *const kPrimer =
    "You will receive prompts in Terse, a compact prompt format. Read them as follows.\n"
    "Lines are `key: value`. Keys: task (action), ctx (background), in (input), err (exact error), "
    "for (audience), tone (voice), ask (request inside a drafted message), why (reason), out (answer "
    "format and length), let (define a name to reuse), if (condition), ref (point to earlier part).\n"
    "Operators: `+` include, `-` exclude, `!` hard rule never broken, `->` becomes/then, `|` or, `?` you "
    "decide, `1. 2.` ordered steps, `=` defines, `~` approximately.\n"
    "Follow `out:` exactly. `out: terse` means no intro, no summary, no filler. `out: terse syntax` means reply "
    "in Terse format too.\n"
    "If a Terse prompt is ambiguous, ask one short question instead of guessing.\n";

namespace {

bool live_field(const Node &n) { return !n.dead && n.kind == NodeKind::Field; }

// "name = value" -> {trim(name), trim(value)}
std::optional<std::pair<std::string, std::string>> split_let(std::string_view v) {
    std::size_t eq = v.find('=');
    if (eq == std::string_view::npos) return std::nullopt;
    return std::make_pair(trim(v.substr(0, eq)), trim(v.substr(eq + 1)));
}

// ---------------------------------------------------------------- terse

void emit_terse(const Prompt &p, std::string &b) {
    for (const Node &n : p.nodes)
        if (!n.dead) b += terse_line(n);
}

// ---------------------------------------------------------------- json

void json_str(std::string &b, std::string_view s) { b += json_quote(s); }

void json_list(std::string &b, const Prompt &p, NodeKind k, std::string_view name, bool &first) {
    bool any = false;
    for (const Node &n : p.nodes)
        if (!n.dead && n.kind == k) { any = true; break; }
    if (!any) return;
    b += cat(first ? "" : ",", "\n  \"", name, "\": [");
    first = false;
    bool f = true;
    for (const Node &n : p.nodes) {
        if (n.dead || n.kind != k) continue;
        b += f ? "\n    " : ",\n    ";
        f = false;
        json_str(b, n.value);
    }
    b += "\n  ]";
}

void emit_json(const Prompt &p, std::string &b) {
    const auto &nodes = p.nodes;
    b += "{";
    bool first = true;
    // fields in order of first appearance; repeated keys are joined with newlines
    for (std::size_t i = 0; i < nodes.size(); i++) {
        const Node &n = nodes[i];
        if (!live_field(n) || n.key == "let") continue;
        bool seen = false;
        for (std::size_t j = 0; j < i && !seen; j++) seen = live_field(nodes[j]) && nodes[j].key == n.key;
        if (seen) continue;
        std::string v;
        for (std::size_t j = i; j < nodes.size(); j++)
            if (live_field(nodes[j]) && nodes[j].key == n.key) {
                if (!v.empty()) v += '\n';
                v += nodes[j].value;
            }
        b += first ? "\n  " : ",\n  ";
        first = false;
        json_str(b, n.key);
        b += ": ";
        json_str(b, v);
    }
    bool any_let = false;
    for (const Node &n : nodes) {
        if (!live_field(n) || n.key != "let") continue;
        auto kv = split_let(n.value);
        if (!kv) continue;
        if (!any_let) { b += cat(first ? "" : ",", "\n  \"let\": {"); first = false; }
        b += any_let ? ",\n    " : "\n    ";
        any_let = true;
        json_str(b, kv->first);
        b += ": ";
        json_str(b, kv->second);
    }
    if (any_let) b += "\n  }";
    json_list(b, p, NodeKind::Plus, "include", first);
    json_list(b, p, NodeKind::Minus, "exclude", first);
    json_list(b, p, NodeKind::Bang, "rules", first);
    json_list(b, p, NodeKind::Step, "steps", first);
    b += "\n}\n";
}

// ---------------------------------------------------------------- english

void sentence(std::string &b, std::string_view prefix, std::string_view v) {
    b += prefix;
    if (v.find('\n') != std::string_view::npos) {
        b += cat('\n', v, '\n');
        return;
    }
    b += v;
    char last = v.empty() ? '.' : v.back();
    if (std::string_view(".!?\"'`)").find(last) == std::string_view::npos) b += '.';
    b += '\n';
}

void joined(std::string &b, const Prompt &p, NodeKind k, std::string_view prefix) {
    std::string v;
    for (const Node &n : p.nodes) {
        if (n.dead || n.kind != k) continue;
        if (!v.empty()) v += "; ";
        v += n.value;
    }
    if (!v.empty()) sentence(b, prefix, v);
}

std::string capitalized(std::string s) {
    if (!s.empty()) s[0] = ascii::to_upper(s[0]);
    return s;
}

void english_field(std::string &b, const Node &n) {
    const std::string &k = n.key;
    const std::string &v = n.value;
    if (k == "task") sentence(b, "", capitalized(v));
    else if (k == "ctx") sentence(b, "Context: ", v);
    else if (k == "in") sentence(b, "Input: ", v);
    else if (k == "err") b += cat("The exact error message is: ", v, '\n');
    else if (k == "for") sentence(b, "Write it for this audience: ", v);
    else if (k == "tone") sentence(b, "Use this tone: ", v);
    else if (k == "ask") sentence(b, "The message should ask for: ", v);
    else if (k == "why") sentence(b, "The reason is: ", v);
    else if (k == "ref") sentence(b, "Refer back to ", v);
    else if (k == "out") sentence(b, "Format the answer as: ", v);
    else if (k == "let") {
        if (auto kv = split_let(v)) sentence(b, cat("\"", kv->first, "\" means: "), kv->second);
    } else if (k == "if") {
        std::size_t arrow = v.find("->");
        if (arrow != std::string::npos)
            sentence(b, cat("If ", trim(std::string_view(v).substr(0, arrow)), ", then "),
                     trim(std::string_view(v).substr(arrow + 2)));
        else
            sentence(b, "If: ", v);
    } else {
        sentence(b, capitalized(cat(k, ": ")), v);
    }
}

void emit_english(const Prompt &p, std::string &b) {
    for (int rank = 0; rank <= 5; rank++) {
        if (rank == 3) {
            joined(b, p, NodeKind::Plus, "Make sure to include: ");
            joined(b, p, NodeKind::Minus, "Do not include: ");
            for (const Node &n : p.nodes)
                if (!n.dead && n.kind == NodeKind::Bang) sentence(b, "Hard rule, never break it: ", n.value);
            continue;
        }
        if (rank == 4) {
            bool any = false;
            for (const Node &n : p.nodes) {
                if (n.dead || n.kind != NodeKind::Step) continue;
                if (!any) b += "Do these steps in order:\n";
                any = true;
                b += cat(n.step, ". ", n.value, '\n');
            }
            continue;
        }
        for (const Node &n : p.nodes)
            if (live_field(n) && node_rank(n) == rank) english_field(b, n);
    }
}

// ---------------------------------------------------------------- shared helpers

bool has_newline(std::string_view v) { return v.find('\n') != std::string_view::npos; }

// Indents every line after the first by `pad` spaces.
std::string hang(std::string_view v, std::size_t pad) {
    std::string out;
    for (char c : v) {
        out += c;
        if (c == '\n') out.append(pad, ' ');
    }
    return out;
}

std::string xml_tag_for(const std::string &key) {
    static const std::pair<const char *, const char *> kTags[] = {
        {"task", "task"},       {"ctx", "context"}, {"in", "input"},       {"err", "error"},
        {"for", "audience"},    {"tone", "tone"},   {"ask", "ask"},        {"why", "reason"},
        {"ref", "reference"},   {"out", "output_format"},
    };
    for (const auto &[k, t] : kTags)
        if (key == k) return t;
    return key;
}

std::string md_label_for(const std::string &key) {
    static const std::pair<const char *, const char *> kLabels[] = {
        {"task", "Task"},   {"ctx", "Context"}, {"in", "Input"}, {"err", "Exact error"},
        {"for", "Audience"}, {"tone", "Tone"},  {"ask", "Ask"},  {"why", "Why"},
        {"ref", "Refer to"}, {"out", "Output format"},
    };
    for (const auto &[k, l] : kLabels)
        if (key == k) return l;
    return capitalized(key);
}

// Nodes in the canonical order used by the english/xml/markdown targets.
template <class F> void for_rank(const Prompt &p, int rank, F f) {
    for (const Node &n : p.nodes)
        if (!n.dead && node_rank(n) == rank) f(n);
}

// ---------------------------------------------------------------- xml

std::string xml_attr(std::string_view v) {
    std::string out;
    for (char c : v) {
        if (c == '"') out += "&quot;";
        else if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else out += c;
    }
    return out;
}

// Text is kept verbatim (not entity-escaped) so code stays readable to the model.
void xml_elem(std::string &b, std::string_view indent, std::string_view tag, std::string_view attrs,
              std::string_view v) {
    b += cat(indent, '<', tag, attrs, '>');
    if (has_newline(v)) b += cat('\n', v, '\n', indent);
    else b += v;
    b += cat("</", tag, ">\n");
}

void xml_list(std::string &b, const Prompt &p, NodeKind k, std::string_view group, std::string_view item) {
    bool any = false;
    for (const Node &n : p.nodes) {
        if (n.dead || n.kind != k) continue;
        if (!any) b += cat('<', group, ">\n");
        any = true;
        xml_elem(b, "  ", item, k == NodeKind::Step ? cat(" n=\"", n.step, '"') : std::string(), n.value);
    }
    if (any) b += cat("</", group, ">\n");
}

void emit_xml(const Prompt &p, std::string &b) {
    for_rank(p, 0, [&](const Node &n) { xml_elem(b, "", xml_tag_for(n.key), "", n.value); });
    bool any_let = false;
    for_rank(p, 1, [&](const Node &n) {
        auto kv = split_let(n.value);
        if (!kv) return;
        if (!any_let) b += "<definitions>\n";
        any_let = true;
        xml_elem(b, "  ", "definition", cat(" name=\"", xml_attr(kv->first), '"'), kv->second);
    });
    if (any_let) b += "</definitions>\n";
    for_rank(p, 2, [&](const Node &n) {
        if (n.key == "if") {
            std::size_t arrow = n.value.find("->");
            if (arrow != std::string::npos) {
                xml_elem(b, "", "if", cat(" condition=\"", xml_attr(trim(std::string_view(n.value).substr(0, arrow))), '"'),
                         trim(std::string_view(n.value).substr(arrow + 2)));
                return;
            }
        }
        xml_elem(b, "", xml_tag_for(n.key), "", n.value);
    });
    xml_list(b, p, NodeKind::Plus, "include", "item");
    xml_list(b, p, NodeKind::Minus, "exclude", "item");
    xml_list(b, p, NodeKind::Bang, "rules", "rule");
    xml_list(b, p, NodeKind::Step, "steps", "step");
    for_rank(p, 5, [&](const Node &n) { xml_elem(b, "", xml_tag_for(n.key), "", n.value); });
}

// ---------------------------------------------------------------- markdown

void md_field(std::vector<std::string> &blocks, const std::string &label, std::string_view v) {
    if (has_newline(v)) blocks.push_back(cat("**", label, ":**\n\n", v));
    else blocks.push_back(cat("**", label, ":** ", v));
}

void md_list(std::vector<std::string> &blocks, const std::string &label, const std::vector<std::string> &items,
             bool numbered) {
    if (items.empty()) return;
    std::string s = cat("**", label, ":**\n");
    for (std::size_t i = 0; i < items.size(); i++) {
        std::string marker = numbered ? cat(i + 1, ". ") : std::string("- ");
        s += cat('\n', marker, hang(items[i], marker.size()));
    }
    blocks.push_back(s);
}

std::vector<std::string> values_of(const Prompt &p, NodeKind k) {
    std::vector<std::string> v;
    for (const Node &n : p.nodes)
        if (!n.dead && n.kind == k) v.push_back(n.value);
    return v;
}

void emit_markdown(const Prompt &p, std::string &b) {
    std::vector<std::string> blocks;
    for_rank(p, 0, [&](const Node &n) { md_field(blocks, md_label_for(n.key), n.value); });
    std::vector<std::string> defs, conds;
    for_rank(p, 1, [&](const Node &n) {
        if (auto kv = split_let(n.value)) defs.push_back(cat('`', kv->first, "`: ", kv->second));
    });
    md_list(blocks, "Definitions", defs, false);
    for_rank(p, 2, [&](const Node &n) {
        std::size_t arrow = n.value.find("->");
        if (n.key == "if" && arrow != std::string::npos)
            conds.push_back(cat("If ", trim(std::string_view(n.value).substr(0, arrow)), ", then ",
                                trim(std::string_view(n.value).substr(arrow + 2))));
        else
            md_field(blocks, md_label_for(n.key), n.value);
    });
    md_list(blocks, "Conditions", conds, false);
    md_list(blocks, "Include", values_of(p, NodeKind::Plus), false);
    md_list(blocks, "Exclude", values_of(p, NodeKind::Minus), false);
    md_list(blocks, "Rules (never break)", values_of(p, NodeKind::Bang), false);
    md_list(blocks, "Steps", values_of(p, NodeKind::Step), true);
    for_rank(p, 5, [&](const Node &n) { md_field(blocks, md_label_for(n.key), n.value); });
    for (std::size_t i = 0; i < blocks.size(); i++) b += cat(i ? "\n\n" : "", blocks[i]);
    if (!blocks.empty()) b += '\n';
}

// ---------------------------------------------------------------- yaml

bool yaml_needs_quotes(std::string_view v) {
    if (v.empty() || v.front() == ' ' || v.back() == ' ' || v.back() == ':') return true;
    if (std::string_view("-?:,[]{}#&*!|>'\"%@`").find(v.front()) != std::string_view::npos) return true;
    if (v.find(": ") != std::string_view::npos || v.find(" #") != std::string_view::npos) return true;
    for (char c : v)
        if (static_cast<unsigned char>(c) < 0x20) return true;
    std::string low;
    for (char c : v) low += ascii::to_lower(c);
    static const char *kReserved[] = {"true", "false", "yes", "no", "on", "off", "y", "n", "null", "~"};
    for (const char *r : kReserved)
        if (low == r) return true;
    // numbers: 12, -3, 1.5, 1e3, 0x1f ...
    std::size_t i = low[0] == '-' || low[0] == '+' ? 1 : 0;
    return i < low.size() && (ascii::is_digit(low[i]) || low[i] == '.') &&
           low.find_first_not_of("0123456789.e+-_xabcdefo", i) == std::string::npos;
}

// A scalar written after "key: " or "- " at column `indent`.
std::string yaml_scalar(std::string_view v, std::size_t indent) {
    if (has_newline(v) && v.front() != ' ' && v.find('\r') == std::string_view::npos) {
        std::string out = "|-";
        std::size_t start = 0;
        while (start <= v.size()) {
            std::size_t e = v.find('\n', start);
            std::string_view line = v.substr(start, e == std::string_view::npos ? std::string_view::npos : e - start);
            out += '\n';
            if (!line.empty()) out += cat(std::string(indent + 2, ' '), line);
            if (e == std::string_view::npos) break;
            start = e + 1;
        }
        return out;
    }
    return yaml_needs_quotes(v) ? json_quote(v) : std::string(v);
}

void yaml_list(std::string &b, const Prompt &p, NodeKind k, std::string_view name) {
    auto items = values_of(p, k);
    if (items.empty()) return;
    b += cat(name, ":\n");
    for (const auto &v : items) b += cat("  - ", yaml_scalar(v, 2), '\n');
}

void emit_yaml(const Prompt &p, std::string &b) {
    const auto &nodes = p.nodes;
    for (std::size_t i = 0; i < nodes.size(); i++) {
        const Node &n = nodes[i];
        if (!live_field(n) || n.key == "let") continue;
        bool seen = false;
        for (std::size_t j = 0; j < i && !seen; j++) seen = live_field(nodes[j]) && nodes[j].key == n.key;
        if (seen) continue;
        std::string v;
        for (std::size_t j = i; j < nodes.size(); j++)
            if (live_field(nodes[j]) && nodes[j].key == n.key) v += cat(v.empty() ? "" : "\n", nodes[j].value);
        b += cat(n.key, ": ", yaml_scalar(v, 0), '\n');
    }
    bool any_let = false;
    for (const Node &n : nodes) {
        if (!live_field(n) || n.key != "let") continue;
        auto kv = split_let(n.value);
        if (!kv) continue;
        if (!any_let) b += "let:\n";
        any_let = true;
        b += cat("  ", yaml_needs_quotes(kv->first) ? json_quote(kv->first) : kv->first, ": ",
                 yaml_scalar(kv->second, 2), '\n');
    }
    yaml_list(b, p, NodeKind::Plus, "include");
    yaml_list(b, p, NodeKind::Minus, "exclude");
    yaml_list(b, p, NodeKind::Bang, "rules");
    yaml_list(b, p, NodeKind::Step, "steps");
}

} // namespace

std::string terse_line(const Node &n) {
    switch (n.kind) {
    case NodeKind::Field: return cat(n.key, ": ", n.value, '\n');
    case NodeKind::Plus: return cat("+ ", n.value, '\n');
    case NodeKind::Minus: return cat("- ", n.value, '\n');
    case NodeKind::Bang: return cat("! ", n.value, '\n');
    case NodeKind::Step: return cat(n.step, ". ", n.value, '\n');
    }
    return {};
}

std::string emit(const Prompt &p, EmitKind kind, bool with_primer) {
    std::string b;
    if (with_primer && kind == EmitKind::Terse) {
        b += kPrimer;
        b += '\n';
    }
    switch (kind) {
    case EmitKind::Terse: emit_terse(p, b); break;
    case EmitKind::Json: emit_json(p, b); break;
    case EmitKind::English: emit_english(p, b); break;
    case EmitKind::Xml: emit_xml(p, b); break;
    case EmitKind::Markdown: emit_markdown(p, b); break;
    case EmitKind::Yaml: emit_yaml(p, b); break;
    }
    return b;
}

// Rough token estimate (no real tokenizer is bundled):
// short words ~1 token, long words ~1 per 6 letters, digits ~1 per 3,
// each punctuation run ~1 per 2 chars, each newline 1, each non-ASCII char ~1.
int approx_tokens(std::string_view s) {
    int t = 0;
    std::size_t i = 0;
    const std::size_t n = s.size();
    auto u = [&](std::size_t k) { return static_cast<unsigned char>(s[k]); };
    while (i < n) {
        const char c = s[i];
        if (ascii::is_alpha(c)) {
            int len = 0;
            while (i < n && ascii::is_alpha(s[i])) { len++; i++; }
            t += len <= 8 ? 1 : (len + 5) / 6;
        } else if (ascii::is_digit(c)) {
            int len = 0;
            while (i < n && ascii::is_digit(s[i])) { len++; i++; }
            t += (len + 2) / 3;
        } else if (c == '\n') {
            t++;
            i++;
        } else if (c == ' ' || c == '\t') {
            i++;
        } else if (u(i) >= 0x80) {
            t++;
            i++;
            while (i < n && (u(i) & 0xC0) == 0x80) i++;
        } else if (ascii::is_punct(c)) {
            int len = 0;
            while (i < n && ascii::is_punct(s[i])) { len++; i++; }
            t += (len + 1) / 2;
        } else {
            i++; // other control characters: free (the C version looped forever here)
        }
    }
    return t;
}

} // namespace terse
