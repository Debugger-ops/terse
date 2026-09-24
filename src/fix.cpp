// fix.cpp — `--fix`: write the optimizer's rewrites back into the source file.
//
// Only values written directly in the top-level file are touched: nothing that came from
// #include, #use, #for or {NAME} substitution, and no multi-line (fenced) values. A value
// is rewritten only if the source still holds the exact original text at its column.
#include "tersec.hpp"

#include <algorithm>

namespace terse {

FixResult apply_fixes(const Prompt &p, const Diagnostics::SrcFile &src, std::string_view raw) {
    SourceText text = split_source(raw);
    struct Edit { std::size_t line; bool remove; std::string before, after; };
    std::vector<Edit> edits;

    for (const Node &n : p.nodes) {
        if (n.loc.exp || n.subst || n.loc.file != src.name || n.loc.line <= 0) continue;
        if (n.orig_value.find('\n') != std::string::npos) continue;
        const std::size_t li = std::size_t(n.loc.line - 1);
        if (li >= text.lines.size() || text.lines[li] != src.lines[li]) continue;
        std::string &line = text.lines[li];
        const std::size_t col = std::size_t(n.vcol - 1);
        if (line.compare(col, n.orig_value.size(), n.orig_value) != 0) continue;
        if (n.dead) {
            edits.push_back(Edit{li, true, line, {}});
        } else if (n.value != n.orig_value) {
            std::string fixed = line.substr(0, col) + n.value + line.substr(col + n.orig_value.size());
            edits.push_back(Edit{li, false, line, fixed});
        }
    }

    FixResult r;
    r.changes = int(edits.size());
    if (edits.empty()) {
        r.text = std::string(raw);
        return r;
    }
    std::sort(edits.begin(), edits.end(), [](const Edit &a, const Edit &b) { return a.line < b.line; });

    r.diff = cat("--- ", src.name, "\n+++ ", src.name, " (fixed)\n");
    int removed = 0;
    for (const Edit &e : edits) {
        const int old_no = int(e.line) + 1, new_no = old_no - removed;
        if (e.remove) {
            r.diff += cat("@@ -", old_no, ",1 +", new_no - 1, ",0 @@\n-", e.before, '\n');
            removed++;
        } else {
            r.diff += cat("@@ -", old_no, " +", new_no, " @@\n-", e.before, "\n+", e.after, '\n');
            text.lines[e.line] = e.after;
        }
    }
    // drop removed lines (back to front so indices stay valid)
    for (auto it = edits.rbegin(); it != edits.rend(); ++it) {
        if (!it->remove) continue;
        text.lines.erase(text.lines.begin() + std::ptrdiff_t(it->line));
        text.eols.erase(text.eols.begin() + std::ptrdiff_t(it->line));
    }
    r.text = text.join();
    return r;
}

} // namespace terse
