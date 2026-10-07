#include "search.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <fstream>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace fixture_search {
namespace {

bool space(char c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f';
}

bool alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

char lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

std::string lowercase(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), lower);
    return value;
}

std::string trim(std::string value) {
    const auto first = value.find_first_not_of(" \t\r\n\f");
    if (first == std::string::npos) return {};
    return value.substr(first, value.find_last_not_of(" \t\r\n\f") - first + 1);
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> result;
    std::string word;
    for (char c : text) {
        if (alnum(c)) {
            word += lower(c);
        } else if (!word.empty()) {
            result.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty()) result.push_back(std::move(word));
    return result;
}

void append_utf8(std::string& out, unsigned codepoint) {
    if (codepoint < 0x80) {
        out += static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        out += static_cast<char>(0xc0 | (codepoint >> 6));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else if (codepoint < 0x10000) {
        out += static_cast<char>(0xe0 | (codepoint >> 12));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (codepoint >> 18));
        out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (codepoint & 0x3f));
    }
}

std::string entities(std::string_view input) {
    std::string result;
    for (std::size_t i = 0; i < input.size(); ++i) {
        if (input[i] != '&') {
            result += input[i];
            continue;
        }
        const auto offset = input.substr(i + 1, 12).find(';');
        if (offset == std::string_view::npos) {
            result += '&';
            continue;
        }
        const auto end = i + 1 + offset;
        const auto entity = input.substr(i + 1, end - i - 1);
        unsigned codepoint = 0;
        if (entity == "amp") codepoint = '&';
        else if (entity == "lt") codepoint = '<';
        else if (entity == "gt") codepoint = '>';
        else if (entity == "quot") codepoint = '"';
        else if (entity == "apos") codepoint = '\'';
        else if (entity == "nbsp") codepoint = ' ';
        else if (!entity.empty() && entity[0] == '#') {
            std::size_t j = 1;
            unsigned base = 10;
            if (j < entity.size() && (entity[j] == 'x' || entity[j] == 'X')) {
                ++j;
                base = 16;
            }
            bool valid = j < entity.size();
            for (; valid && j < entity.size(); ++j) {
                const char c = lower(entity[j]);
                const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
                                       c >= 'a' && c <= 'f' ? c - 'a' + 10 : 16;
                if (digit >= base || codepoint > (0x10ffff - digit) / base) {
                    valid = false;
                } else {
                    codepoint = codepoint * base + digit;
                }
            }
            if (!valid || (codepoint >= 0xd800 && codepoint <= 0xdfff)) codepoint = 0;
        }
        if (codepoint != 0) {
            append_utf8(result, codepoint);
            i = end;
        } else {
            result += '&';
        }
    }
    return result;
}

struct Html {
    std::string title;
    std::string text;
    std::vector<std::string> links;
    std::size_t link_count = 0;
};

std::size_t tag_end(const std::string& html, std::size_t start) {
    char quote = 0;
    for (std::size_t i = start; i < html.size(); ++i) {
        if (quote) {
            if (html[i] == quote) quote = 0;
        } else if (html[i] == '\'' || html[i] == '"') {
            quote = html[i];
        } else if (html[i] == '>') {
            return i;
        }
    }
    return std::string::npos;
}

std::size_t closing_tag(const std::string& lowercase_html, const std::string& name,
                        std::size_t from) {
    const std::string prefix = "</" + name;
    for (auto pos = lowercase_html.find(prefix, from); pos != std::string::npos;
         pos = lowercase_html.find(prefix, pos + prefix.size())) {
        const auto after = pos + prefix.size();
        if (after == lowercase_html.size() || space(lowercase_html[after]) ||
            lowercase_html[after] == '>') return pos;
    }
    return std::string::npos;
}

Html parse_html(const std::string& html, std::size_t link_capacity) {
    Html result;
    const auto folded = lowercase(html);
    bool in_title = false;
    std::size_t i = 0;
    while (i < html.size()) {
        if (html[i] != '<') {
            auto end = html.find('<', i);
            if (end == std::string::npos) end = html.size();
            const auto text = entities(std::string_view(html).substr(i, end - i));
            result.text += text;
            if (in_title) result.title += text;
            i = end;
            continue;
        }
        result.text += ' ';
        if (in_title) result.title += ' ';
        if (html.compare(i, 4, "<!--") == 0) {
            const auto end = html.find("-->", i + 4);
            i = end == std::string::npos ? html.size() : end + 3;
            continue;
        }
        const auto end = tag_end(html, i + 1);
        if (end == std::string::npos) break;
        std::size_t p = i + 1;
        while (p < end && space(html[p])) ++p;
        const bool closing = p < end && html[p] == '/';
        if (closing) ++p;
        const auto name_start = p;
        while (p < end && (alnum(html[p]) || html[p] == '-')) ++p;
        const auto name = folded.substr(name_start, p - name_start);
        if (name == "title") in_title = !closing;
        if (!closing && (name == "script" || name == "style")) {
            const auto close = closing_tag(folded, name, end + 1);
            i = close == std::string::npos ? html.size() : close;
            continue;
        }
        if (!closing && name == "a") {
            while (p < end) {
                while (p < end && (space(html[p]) || html[p] == '/')) ++p;
                const auto attribute_start = p;
                while (p < end && !space(html[p]) && html[p] != '/' && html[p] != '=') ++p;
                if (p == attribute_start) { ++p; continue; }
                const auto attribute = folded.substr(attribute_start, p - attribute_start);
                while (p < end && space(html[p])) ++p;
                if (p == end || html[p] != '=') continue;
                ++p;
                while (p < end && space(html[p])) ++p;
                char quote = 0;
                if (p < end && (html[p] == '\'' || html[p] == '"')) quote = html[p++];
                const auto value_start = p;
                while (p < end && (quote ? html[p] != quote : !space(html[p]))) ++p;
                const auto value = std::string_view(html).substr(value_start, p - value_start);
                if (quote && p < end) ++p;
                if (attribute == "href") {
                    ++result.link_count;
                    if (result.links.size() < link_capacity) result.links.push_back(entities(value));
                    break;
                }
            }
        }
        i = end + 1;
    }
    result.title = trim(result.title);
    for (char& c : result.title) if (space(c)) c = ' ';
    return result;
}

bool within(const std::filesystem::path& path, const std::filesystem::path& root) {
    auto item = path.begin();
    for (const auto& component : root) {
        if (item == path.end()) return false;
        if (*item != component) return false;
        ++item;
    }
    return true;
}

std::filesystem::path canonical_existing(const std::filesystem::path& path, std::error_code& ec) {
#ifdef _WIN32
    // Resolve intermediate directory links too; some MinGW canonical() versions do not.
    const auto raw = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                 nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return {};
    }
    struct Handle {
        HANDLE value;
        ~Handle() { CloseHandle(value); }
    } handle{raw};
    const auto needed = GetFinalPathNameByHandleW(handle.value, nullptr, 0, FILE_NAME_NORMALIZED);
    if (!needed) {
        ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
        return {};
    }
    std::wstring final_path(needed, L'\0');
    const auto length = GetFinalPathNameByHandleW(handle.value, final_path.data(), needed,
                                                FILE_NAME_NORMALIZED);
    if (!length || length >= needed) {
        ec = std::error_code(static_cast<int>(length ? ERROR_INSUFFICIENT_BUFFER : GetLastError()),
                             std::system_category());
        return {};
    }
    final_path.resize(length);
    if (final_path.compare(0, 8, L"\\\\?\\UNC\\") == 0) final_path = L"\\\\" + final_path.substr(8);
    else if (final_path.compare(0, 4, L"\\\\?\\") == 0) final_path.erase(0, 4);
    ec.clear();
    return std::filesystem::path(final_path);
#else
    return std::filesystem::canonical(path, ec);
#endif
}

struct Resolved {
    std::filesystem::path path;
    std::string reason;
};

Resolved resolve(const std::filesystem::path& root, const std::filesystem::path& source,
                 std::string href) {
    href = trim(std::move(href));
    const auto suffix = href.find_first_of("?#");
    if (suffix != std::string::npos) href.resize(suffix);
    if (href.find_first_of(":\\%") != std::string::npos || href.find('\0') != std::string::npos ||
        href.compare(0, 2, "//") == 0) return {{}, "unsupported_url"};
    std::filesystem::path candidate;
    if (href.empty()) candidate = source;
    else if (href.front() == '/') candidate = root / std::filesystem::u8path(href.substr(1));
    else candidate = source.parent_path() / std::filesystem::u8path(href);
    candidate = candidate.lexically_normal();
    if (!within(candidate, root)) return {{}, "outside_root"};
    std::error_code ec;
    const auto canonical = canonical_existing(candidate, ec);
    if (ec) return {{}, "unavailable_path"};
    if (!within(canonical, root)) return {{}, "outside_root"};
    if (!std::filesystem::is_regular_file(canonical, ec) || ec) return {{}, "not_regular_file"};
    const auto extension = lowercase(canonical.extension().u8string());
    if (extension != ".html" && extension != ".htm") return {{}, "not_html"};
    return {canonical, {}};
}

std::string identity(const std::filesystem::path& path) {
    return path.generic_u8string();
}

std::string read_page(const std::filesystem::path& path, std::size_t capacity) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("read_error");
    std::string content;
    std::array<char, 8192> buffer{};
    while (content.size() < capacity && input) {
        const auto length = std::min(buffer.size(), capacity - content.size());
        input.read(buffer.data(), static_cast<std::streamsize>(length));
        content.append(buffer.data(), static_cast<std::size_t>(input.gcount()));
    }
    if (input.bad()) throw std::runtime_error("read_error");
    if (input.peek() != std::char_traits<char>::eof()) throw std::runtime_error("file_changed");
    return content;
}

} // namespace

void Index::add(std::string path, std::string title, const std::string& text) {
    const auto words = tokenize(text);
    std::unordered_map<std::string, std::size_t> frequencies;
    for (const auto& word : words) ++frequencies[word];
    const auto document = documents_.size();
    for (const auto& entry : frequencies) postings_[entry.first].push_back({document, entry.second});
    tokens_ += words.size();
    documents_.push_back({std::move(path), std::move(title), words.size()});
}

std::vector<Hit> Index::search(const std::string& query) const {
    auto words = tokenize(query);
    std::sort(words.begin(), words.end());
    words.erase(std::unique(words.begin(), words.end()), words.end());
    if (words.empty()) return {};
    std::vector<const std::vector<Posting>*> lists;
    for (const auto& word : words) {
        const auto found = postings_.find(word);
        if (found == postings_.end()) return {};
        lists.push_back(&found->second);
    }
    std::sort(lists.begin(), lists.end(), [](const auto* a, const auto* b) {
        return a->size() < b->size();
    });
    std::vector<Hit> hits;
    for (const auto& posting : *lists.front()) hits.push_back({posting.document, posting.frequency});
    for (std::size_t term = 1; term < lists.size() && !hits.empty(); ++term) {
        std::vector<Hit> intersection;
        std::size_t a = 0, b = 0;
        const auto& list = *lists[term];
        while (a < hits.size() && b < list.size()) {
            if (hits[a].document < list[b].document) ++a;
            else if (hits[a].document > list[b].document) ++b;
            else {
                intersection.push_back({hits[a].document, hits[a].score + list[b].frequency});
                ++a;
                ++b;
            }
        }
        hits = std::move(intersection);
    }
    std::sort(hits.begin(), hits.end(), [this](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;
        return documents_[a.document].path < documents_[b.document].path;
    });
    return hits;
}

CrawlResult crawl(const std::filesystem::path& supplied_root, const std::string& start,
                  const Limits& limits) {
    if (!limits.pages || !limits.page_bytes || !limits.total_bytes) {
        throw std::invalid_argument("page and byte limits must be positive");
    }
    std::error_code ec;
    const auto root = canonical_existing(supplied_root, ec);
    if (ec || !std::filesystem::is_directory(root, ec) || ec) {
        throw std::invalid_argument("root must be an existing directory");
    }
    const auto seed = resolve(root, root / "index.html", start);
    if (!seed.reason.empty()) throw std::invalid_argument("invalid start page: " + seed.reason);
    struct Pending { std::filesystem::path path; std::size_t depth; };
    std::deque<Pending> frontier{{seed.path, 0}};
    std::unordered_set<std::string> scheduled{identity(seed.path)};
    CrawlResult result;
    std::size_t links_considered = 0;
    while (!frontier.empty()) {
        const auto item = frontier.front();
        frontier.pop_front();
        const auto name = item.path.lexically_relative(root).generic_u8string();
        if (result.index.documents().size() == limits.pages) {
            result.skipped.push_back({name, "page_limit"});
            continue;
        }
        const auto bytes = std::filesystem::file_size(item.path, ec);
        if (ec) { result.skipped.push_back({name, "read_error"}); continue; }
        if (bytes > limits.page_bytes) {
            result.skipped.push_back({name, "page_byte_limit"});
            continue;
        }
        if (bytes > limits.total_bytes - result.bytes_indexed) {
            result.skipped.push_back({name, "total_byte_limit"});
            continue;
        }
        std::string content;
        try {
            content = read_page(item.path, static_cast<std::size_t>(bytes));
        } catch (const std::runtime_error& error) {
            result.skipped.push_back({name, error.what()});
            continue;
        }
        const auto html = parse_html(content, limits.links - links_considered);
        result.bytes_indexed += content.size();
        result.index.add(name, html.title.empty() ? name : html.title, html.text);
        result.links_seen += html.link_count;
        result.links_omitted += html.link_count - html.links.size();
        links_considered += html.links.size();
        for (const auto& link : html.links) {
            const auto target = resolve(root, item.path, link);
            if (!target.reason.empty()) {
                result.skipped.push_back({link, target.reason});
                continue;
            }
            if (!scheduled.insert(identity(target.path)).second) {
                ++result.duplicate_links;
                continue;
            }
            const auto target_name = target.path.lexically_relative(root).generic_u8string();
            if (item.depth == limits.depth) {
                result.skipped.push_back({target_name, "depth_limit"});
            } else {
                frontier.push_back({target.path, item.depth + 1});
            }
        }
    }
    return result;
}

} // namespace fixture_search
