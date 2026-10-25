#pragma once

//DH1KLM The canon table (style guide, RFC #6226): a QTableView or QTableWidget on
//DH1KLM color.canon.nested with color.canon.line rules and border, a
//DH1KLM color.canon.control selection band and cyan header text on
//DH1KLM color.canon.raised. Template text for ThemeManager::applyStyleSheet() or
//DH1KLM applyCanonSheet(); a window appends its own sizes and paddings after it.
//DH1KLM
//DH1KLM The selected-item rule sets no text color, so an item's own foreground
//DH1KLM survives selection where a delegate keeps it (the multiFLEX Dashboard); a
//DH1KLM window that wants plain ink on its selection band adds that itself.

#include <QString>

namespace AetherSDR {

inline QString canonTableRules()
{
    return QStringLiteral(R"(
QTableView {
    color: {{color.canon.ink}};
    background: {{color.canon.nested}};
    alternate-background-color: {{color.canon.raised}};
    border: 1px solid {{color.canon.line}};
    border-radius: 6px;
    gridline-color: {{color.canon.line}};
}
QTableView::item:selected {
    background: {{color.canon.control}};
}
QHeaderView::section {
    color: {{color.canon.cyan}};
    background: {{color.canon.raised}};
    border: none;
    border-bottom: 1px solid {{color.canon.line}};
    font-weight: 700;
}
)");
}

} //DH1KLM namespace AetherSDR
