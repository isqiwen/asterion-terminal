#pragma once
#include <string>

namespace asterion::terminal {

inline std::string report_html_text(const std::string& value)
{
    std::string out;
    for (const auto ch : value) {
        switch (ch) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out += ch;
        }
    }
    return out;
}

inline std::string report_csv_cell(std::string value, bool numeric = false)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (!numeric && first != std::string::npos
        && std::string("=+-@").find(value[first]) != std::string::npos)
        value.insert(value.begin(), '\'');
    std::string out = "\"";
    for (const auto ch : value) {
        if (ch == '"')
            out += '"';
        out += ch;
    }
    return out + "\"";
}

} // namespace asterion::terminal
