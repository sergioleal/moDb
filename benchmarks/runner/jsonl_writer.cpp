#include "runner/jsonl_writer.hpp"

#include <system_error>

namespace modb::bench {

JsonlWriter::~JsonlWriter() {
    if (open_ && stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
}

bool JsonlWriter::open(const std::filesystem::path& final_path) {
    if (std::filesystem::exists(final_path)) {
        return false;
    }
    final_path_ = final_path;
    partial_path_ = final_path;
    partial_path_ += ".partial";
    if (std::filesystem::exists(partial_path_)) {
        std::error_code ec;
        std::filesystem::remove(partial_path_, ec);
    }
    stream_.open(partial_path_, std::ios::binary | std::ios::trunc | std::ios::out);
    if (!stream_) {
        return false;
    }
    open_ = true;
    sequence_ = 0;
    bytes_written_ = 0;
    content_buffer_.clear();
    return true;
}

bool JsonlWriter::write_line(std::string_view json_object) {
    if (!open_ || !stream_) {
        return false;
    }
    stream_.write(json_object.data(), static_cast<std::streamsize>(json_object.size()));
    stream_.put('\n');
    stream_.flush();
    if (!stream_) {
        return false;
    }
    content_buffer_.append(json_object.data(), json_object.size());
    content_buffer_.push_back('\n');
    bytes_written_ += json_object.size() + 1;
    return true;
}

bool JsonlWriter::finish() {
    if (!open_) {
        return false;
    }
    stream_.flush();
    stream_.close();
    open_ = false;
    std::error_code ec;
    std::filesystem::rename(partial_path_, final_path_, ec);
    if (ec) {
        return false;
    }
    return true;
}

void JsonlWriter::abandon() {
    if (stream_.is_open()) {
        stream_.flush();
        stream_.close();
    }
    open_ = false;
}

std::string JsonlWriter::content_sha256_hex() const {
    return sha256_hex(sha256_text(content_buffer_));
}

std::filesystem::path make_result_filename(std::string_view utc_stamp,
                                           std::string_view commit_short,
                                           std::string_view host_token) {
    std::string name = "modb-benchmark-";
    name.append(utc_stamp);
    name.push_back('-');
    name.append(commit_short);
    name.push_back('-');
    name.append(host_token);
    name += ".jsonl";
    return name;
}

} // namespace modb::bench
