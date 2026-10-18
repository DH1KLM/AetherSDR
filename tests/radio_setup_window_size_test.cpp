// Radio Setup resizes in both dimensions and preserves current/legacy geometry.
// No hardware or transport: a bare RadioModel, never connected.

#include "TestSettingsProfile.h"
#include "core/AppSettings.h"
#include "gui/RadioSetupDialog.h"
#include "models/RadioModel.h"

#include <QApplication>
#include <QtTest>

using namespace AetherSDR;

namespace {
const char* const kGeometryKey = "RadioSetupDialogGeometry";
}

class RadioSetupWindowSizeTest : public QObject {
    Q_OBJECT
private slots:

    void bothDimensionsAreResizable()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        QCOMPARE(dialog.minimumWidth(), 800);
        QCOMPARE(dialog.maximumWidth(), QWIDGETSIZE_MAX);
        QCOMPARE(dialog.minimumHeight(), 600);
        QCOMPARE(dialog.maximumHeight(), QWIDGETSIZE_MAX);
    }

    void aResizeChangesBothDimensions()
    {
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        dialog.resize(1300, 900);
        QTRY_COMPARE(dialog.size(), QSize(1300, 900));
        dialog.resize(700, 800);
        QTRY_COMPARE(dialog.size(), QSize(800, 800));
    }

    void aWiderSavedGeometryIsRestored()
    {
        auto& settings = AppSettings::instance();
        // Saved by the window when its width was free: x,y,w,h.
        settings.setValue(QLatin1String(kGeometryKey), QStringLiteral("100,100,1400,850"));
        RadioModel model;
        RadioSetupDialog dialog(&model);
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        // Both dimensions, rather than a forced width, survive restoration.
        QTRY_COMPARE(dialog.height(), 850);
        QCOMPARE(dialog.width(), 1400);
        dialog.hide();
        settings.remove(QLatin1String(kGeometryKey));
    }

    void aWiderLegacyBlobIsRestored()
    {
        auto& settings = AppSettings::instance();
        // What the PersistentDialog saved before the move: saveGeometry() as
        // base64, here from a free-width window at 1400x750. (restoreGeometry()
        // fits a blob to the screen, so the height is one the offscreen
        // screen holds.)
        QByteArray blob;
        {
            QWidget wide;
            wide.setGeometry(100, 100, 1400, 750);
            wide.show();
            QVERIFY(QTest::qWaitForWindowExposed(&wide));
            QCOMPARE(wide.width(), 1400);   // CONTROL: the blob is wider than 960
            blob = wide.saveGeometry();
        }
        settings.setValue(QLatin1String(kGeometryKey), QString::fromLatin1(blob.toBase64()));
        RadioModel model;
        RadioSetupDialog dialog(&model);
        QWidget reference(nullptr, dialog.windowFlags());
        reference.setMinimumSize(800,600);
        QVERIFY(reference.restoreGeometry(blob));
        const int expectedWidth = reference.width();
        dialog.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        // Qt may fit a legacy blob to the screen; use its own restore as control.
        QTRY_COMPARE(dialog.height(), 750);
        QCOMPARE(dialog.width(), expectedWidth);
        dialog.hide();
        settings.remove(QLatin1String(kGeometryKey));
    }
};

int main(int argc, char** argv)
{
    TestSettingsProfile profile(QStringLiteral("radio-setup-window-size"));
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    RadioSetupWindowSizeTest test;
    return QTest::qExec(&test, argc, argv);
}
#include "radio_setup_window_size_test.moc"
