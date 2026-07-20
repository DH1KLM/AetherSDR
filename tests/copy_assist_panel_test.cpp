// Offline UI test for CopyAssistPanel (RFC #4333, Phase 5). Runs offscreen
// (QT_QPA_PLATFORM=offscreen) and verifies the confidence color-coding (the
// ggmorse-style feature), tier selection, the enable signal, and clear.

#include "gui/CopyAssistPanel.h"

#include <QApplication>
#include <QSignalSpy>
#include <QTextEdit>

#include <cstdio>

using namespace AetherSDR;

namespace {

int g_failures = 0;

void expect(bool condition, const char* description)
{
    std::printf("%s %s\n", condition ? "[ OK ]" : "[FAIL]", description);
    if (!condition) {
        ++g_failures;
    }
}

QString transcriptHtml(CopyAssistPanel& panel)
{
    // The transcript is the only QTextEdit in the panel.
    QTextEdit* edit = panel.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
    return edit != nullptr ? edit->toHtml() : QString();
}

QString transcriptText(CopyAssistPanel& panel)
{
    QTextEdit* edit = panel.findChild<QTextEdit*>(QStringLiteral("CopyAssistTranscript"));
    return edit != nullptr ? edit->toPlainText() : QString();
}

} // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);

    CopyAssistPanel panel;

    // ---- Confidence color-coding (green=high … red=low) -------------------
    panel.appendText(QStringLiteral("high"), 0.95f);
    panel.appendText(QStringLiteral("medium"), 0.72f);
    panel.appendText(QStringLiteral("low"), 0.50f);
    panel.appendText(QStringLiteral("verylow"), 0.10f);

    const QString html = transcriptHtml(panel).toLower();
    expect(html.contains("high") && html.contains("#00ff88"), "high confidence -> green");
    expect(html.contains("medium") && html.contains("#e0e040"), "medium confidence -> yellow");
    expect(html.contains("low") && html.contains("#ff9020"), "low confidence -> orange");
    expect(html.contains("verylow") && html.contains("#ff4040"), "very low confidence -> red");

    // ---- Empty/whitespace text is ignored ---------------------------------
    const QString before = transcriptText(panel);
    panel.appendText(QStringLiteral("   "), 0.9f);
    expect(transcriptText(panel) == before, "blank utterance is not appended");

    // ---- Clear -------------------------------------------------------------
    panel.clearText();
    expect(transcriptText(panel).trimmed().isEmpty(), "clearText empties the transcript");

    // ---- Tier selection emits the tier id ---------------------------------
    panel.addTier(QStringLiteral("base"), QStringLiteral("Base"));
    panel.addTier(QStringLiteral("small"), QStringLiteral("Small"));
    QSignalSpy tierSpy(&panel, &CopyAssistPanel::tierChanged);
    panel.setCurrentTier(QStringLiteral("small"));
    expect(panel.currentTier() == QStringLiteral("small"), "setCurrentTier selects the tier");
    expect(!tierSpy.isEmpty()
               && tierSpy.last().at(0).toString() == QStringLiteral("small"),
           "tierChanged carries the tier id");

    // ---- Enable toggle emits + reflects state -----------------------------
    QSignalSpy enableSpy(&panel, &CopyAssistPanel::enableToggled);
    panel.setAsrEnabled(true);
    expect(panel.isAsrEnabled(), "setAsrEnabled reflects state");
    expect(!enableSpy.isEmpty() && enableSpy.last().at(0).toBool(),
           "enableToggled(true) emitted");

    std::printf(g_failures == 0 ? "\nCopy Assist panel: ALL PASS\n"
                                : "\nCopy Assist panel: %d FAILURE(S)\n",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
