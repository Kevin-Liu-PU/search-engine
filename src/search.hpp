#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace fixture_search {

struct Limits {
    std::size_t pages = 64;
    std::size_t depth = 8;
    std::size_t page_bytes = 262144;
    std::size_t total_bytes = 4194304;
    std::size_t links = 4096;
};

struct Document {
    std::string path;
    std::string title;
    std::size_t tokens;
};

struct Hit {
    std::size_t document;
    std::size_t score;
};

class Index {
public:
    void add(std::string path, std::string title, const std::string& text);
    std::vector<Hit> search(const std::string& query) const;
    const std::vector<Document>& documents() const { return documents_; }
    std::size_t terms() const { return postings_.size(); }
    std::size_t tokens() const { return tokens_; }

private:
    struct Posting {
        std::size_t document;
        std::size_t frequency;
    };
    std::vector<Document> documents_;
    std::unordered_map<std::string, std::vector<Posting>> postings_;
    std::size_t tokens_ = 0;
};

struct Skipped {
    std::string target;
    std::string reason;
};

struct CrawlResult {
    Index index;
    std::size_t bytes_indexed = 0;
    std::size_t links_seen = 0;
    std::size_t links_omitted = 0;
    std::size_t duplicate_links = 0;
    std::vector<Skipped> skipped;
};

CrawlResult crawl(const std::filesystem::path& root, const std::string& start,
                  const Limits& limits);

} // namespace fixture_search
