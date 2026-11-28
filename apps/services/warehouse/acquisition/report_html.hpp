#pragma once
#include <lexbor/html/html.h>
#include <memory>
#include <string>
#include <string_view>

namespace asterion::warehouse::acquisition::html {

using HtmlDocument = std::unique_ptr<lxb_html_document_t, decltype(&lxb_html_document_destroy)>;

inline bool is_tag(lxb_dom_node_t* node, lxb_tag_id_t tag)
{
    return node->type == LXB_DOM_NODE_TYPE_ELEMENT && lxb_dom_node_tag_id(node) == tag;
}

inline lxb_dom_node_t* first_child_tag(lxb_dom_node_t* parent, lxb_tag_id_t tag)
{
    for (auto* child = parent ? parent->first_child : nullptr; child; child = child->next)
        if (is_tag(child, tag))
            return child;
    return nullptr;
}

inline std::string text(lxb_dom_node_t* node)
{
    std::size_t length = 0;
    auto* raw = lxb_dom_node_text_content(node, &length);
    if (!raw)
        return {};
    std::string_view view(reinterpret_cast<const char*>(raw), length);
    while (!view.empty()) {
        if (view.front() == ' ' || view.front() == '\t' || view.front() == '\r'
            || view.front() == '\n')
            view.remove_prefix(1);
        else if (view.starts_with("\xC2\xA0"))
            view.remove_prefix(2);
        else
            break;
    }
    while (!view.empty()) {
        if (view.back() == ' ' || view.back() == '\t' || view.back() == '\r' || view.back() == '\n')
            view.remove_suffix(1);
        else if (view.ends_with("\xC2\xA0"))
            view.remove_suffix(2);
        else
            break;
    }
    std::string result(view);
    lxb_dom_document_destroy_text_noi(node->owner_document, raw);
    return result;
}

} // namespace asterion::warehouse::acquisition::html
