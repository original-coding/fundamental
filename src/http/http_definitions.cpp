#include "http_definitions.hpp"

#include <unordered_map>

namespace network::http
{

static std::unordered_map<std::string, std::string> s_extension_to_mime_str = {
    { ".txt", "text/plain" },
    { ".csv", "text/csv" },
    { ".log", "text/plain" },
    { ".md", "text/markdown" },
    { ".xml", "application/xml" },
    { ".json", "application/json" },
    { ".jsonc", "application/json" },
    { ".html", "text/html" },
    { ".htm", "text/html" },
    { ".css", "text/css" },
    { ".js", "application/javascript" },
    { ".jpg", "image/jpeg" },
    { ".jpeg", "image/jpeg" },
    { ".png", "image/png" },
    { ".gif", "image/gif" },
    { ".bmp", "image/bmp" },
    { ".tiff", "image/tiff" },
    { ".tif", "image/tiff" },
    { ".svg", "image/svg+xml" },
    { ".webp", "image/webp" },
    { ".mp3", "audio/mpeg" },
    { ".wav", "audio/wav" },
    { ".ogg", "audio/ogg" },
    { ".aac", "audio/aac" },
    { ".flac", "audio/flac" },
    { ".mp4", "video/mp4" },
    { ".avi", "video/x-msvideo" },
    { ".mov", "video/quicktime" },
    { ".wmv", "video/x-ms-wmv" },
    { ".mkv", "video/x-matroska" },
    { ".flv", "video/x-flv" },
    { ".zip", "application/zip" },
    { ".tar", "application/x-tar" },
    { ".gz", "application/gzip" },
    { ".rar", "application/x-rar-compressed" },
    { ".7z", "application/x-7z-compressed" },
    { ".data", "application/octet-stream" },
    { ".exe", "application/octet-stream" },
    { ".msi", "application/x-msdownload" },
    { ".apk", "application/vnd.android.package-archive" },
    { ".dmg", "application/x-apple-diskimage" },
    { ".pdf", "application/pdf" },
    { ".ppt", "application/vnd.ms-powerpoint" },
    { ".pptx", "application/vnd.ms-powerpoint" },
    { ".doc", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
    { ".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document" },
    { ".xls", "application/application/vnd.openxmlformats-officedocument.spreadsheetml.shee" },
    { ".xlsx", "application/application/vnd.openxmlformats-officedocument.spreadsheetml.shee" },

};

std::string ExtensionToType(const std::string& extension) {
    auto iter = s_extension_to_mime_str.find(extension);
    if (iter != s_extension_to_mime_str.end()) return iter->second;

    return "application/octet-stream";
}


} // namespace network::http