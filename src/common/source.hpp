#pragma once

#include <algorithm>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace transom {

struct Span {
    uint32_t line = 1;
    uint32_t col = 1;
    uint32_t offset = 0;
    uint32_t length = 0;
};

enum class DiagLevel { Error, Warning };

struct Diagnostic {
    DiagLevel level = DiagLevel::Error;
    Span span;
    std::string message;
    std::string file; // optional override for multi-file imports

    std::string format(const std::string& defaultFile = "<input>",
                       const std::string& source = "") const {
        const std::string& fname = file.empty() ? defaultFile : file;
        std::ostringstream oss;
        oss << fname << ":" << span.line << ":" << span.col << ": "
            << (level == DiagLevel::Error ? "error: " : "warning: ")
            << message;

        if (!source.empty() && span.line >= 1) {
            std::string lineText = extractLine(source, span.line);
            if (!lineText.empty()) {
                std::string lineNum = std::to_string(span.line);
                std::string pad(lineNum.size(), ' ');
                oss << "\n  " << lineNum << " | " << lineText
                    << "\n  " << pad << " | ";
                uint32_t caretPos = (span.col > 0) ? (span.col - 1) : 0;
                for (uint32_t i = 0; i < caretPos; ++i) {
                    oss << ((i < lineText.size() && lineText[i] == '\t') ? '\t' : ' ');
                }
                oss << '^';
                uint32_t tildeCount = (span.length > 1) ? std::min<uint32_t>(span.length - 1, 40) : 0;
                for (uint32_t i = 0; i < tildeCount; ++i) oss << '~';
            }
        }
        return oss.str();
    }

private:
    static std::string extractLine(const std::string& src, uint32_t targetLine) {
        uint32_t cur = 1;
        size_t start = 0;
        for (size_t i = 0; i <= src.size(); ++i) {
            if (i == src.size() || src[i] == '\n') {
                if (cur == targetLine) {
                    size_t len = i - start;
                    if (len > 0 && src[start + len - 1] == '\r') len--;
                    return src.substr(start, len);
                }
                cur++;
                start = i + 1;
            }
        }
        return "";
    }
};

class DiagnosticEngine {
public:
    void setCurrentFile(std::string file) { currentFile_ = std::move(file); }

    void error(Span span, std::string msg) {
        diags_.push_back({DiagLevel::Error, span, std::move(msg), currentFile_});
        hasErrors_ = true;
    }

    void warning(Span span, std::string msg) {
        diags_.push_back({DiagLevel::Warning, span, std::move(msg), currentFile_});
    }

    bool hasErrors() const { return hasErrors_; }
    const std::vector<Diagnostic>& diagnostics() const { return diags_; }

    std::string formatAll(const std::string& filename = "<input>",
                          const std::string& source = "") const {
        std::ostringstream oss;
        for (const auto& d : diags_) {
            const std::string& src = (d.file.empty() || d.file == filename) ? source : "";
            oss << d.format(filename, src) << "\n";
        }
        return oss.str();
    }

    void clear() {
        diags_.clear();
        hasErrors_ = false;
    }

private:
    std::string currentFile_;
    std::vector<Diagnostic> diags_;
    bool hasErrors_ = false;
};

} // namespace transom
