import {{- import.ContourTerminal -}}
import {{- import.QtQuick -}}
import {{- import.QtQuickControls -}}
import {{- import.QtQuickLayouts -}}
import {{- import.QtMultimedia -}}
import {{- import.QtQuickWindow -}}
import {{- import.QtGraphicalEffects -}}

ContourTerminal
{
    property url bellSoundSource: "qrc:/contour/bell.wav"

    signal showNotification(string title, string content)

    id: vtWidget
    visible: true

    session: terminalSessions.createSession()

    Rectangle {
        id : backgroundColor
        anchors.centerIn: parent
        width:  vtWidget.width
        height:  vtWidget.height
        color: session.backgroundColor
        visible : true
        focus : false
    }

    Image {
        id: backgroundImage
        width:  vtWidget.width
        height:  vtWidget.height
        opacity : session.opacityBackground
        focus: false
        visible : session.isImageBackground
        source :  session.pathToBackground
    }

    FastBlur {
        visible: session.isBlurBackground
        anchors.fill: backgroundImage
        source: backgroundImage
        radius: 32
    }


    ScrollBar {
        id: vbar
        anchors.top: parent.top
        anchors.right : session.isScrollbarRight ? parent.right : undefined
        anchors.left : session.isScrollbarRight ? undefined : parent.left
        anchors.bottom: parent.bottom
        visible : session.isScrollbarVisible ? true : false
        orientation: Qt.Vertical
        policy : ScrollBar.AlwaysOn
        minimumSize : 0.1
        size : vtWidget.session.pageLineCount / (vtWidget.session.pageLineCount + vtWidget.session.historyLineCount)
        stepSize : 1.0 / (vtWidget.session.pageLineCount + vtWidget.session.historyLineCount)
    }

    SoundEffect {
        id: bellSoundEffect
        source: vtWidget.bellSoundSource
    }

    RequestPermission {
        id: requestFontChangeDialog
        text: "The host application is requesting to change the display font."
        onYesClicked: {
            const rememberChoice = requestFontChangeDialog.clickedRememberChoice();
            console.log("[Terminal] Allowing font change request.", rememberChoice ? "to all" : "single", vtWidget.session)
            vtWidget.session.applyPendingFontChange(true, rememberChoice);
        }
        onNoClicked: {
            const rememberChoice = requestFontChangeDialog.clickedRememberChoice();
            console.log("[Terminal] Denying font change request.", rememberChoice ? "to all" : "single", vtWidget.session)
            vtWidget.session.applyPendingFontChange(false, rememberChoice);
        }
        onRejected: {
            console.log("[Terminal] font change request rejected.", vtWidget.session)
            if (vtWidget.session !== null)
                vtWidget.session.applyPendingFontChange(false, false);
        }
    }

    RequestPermission {
        id: requestBufferCaptureDialog
        text: "The host application is requesting to capture the terminal buffer."
        onYesClicked: {
            const rememberChoice = requestBufferCaptureDialog.clickedRememberChoice();
            console.log("[Terminal] Allowing font change request.", rememberChoice ? "to all" : "single")
            vtWidget.session.executePendingBufferCapture(true, rememberChoice);
        }
        onNoClicked: {
            const rememberChoice = requestBufferCaptureDialog.clickedRememberChoice();
            console.log("[Terminal] Denying font change request.", rememberChoice ? "to all" : "single")
            vtWidget.session.executePendingBufferCapture(false, rememberChoice);
        }
        onRejected: {
            console.log("[Terminal] Buffer capture request rejected.")
            vtWidget.session.executePendingBufferCapture(false, false);
        }
    }

    RequestPermission {
        id: requestShowHostWritableStatusLine
        text: "The host application is requesting to show the host-writable statusline."
        onYesClicked: {
            const rememberChoice = requestShowHostWritableStatusLine.clickedRememberChoice();
            console.log("[Terminal] Allowing font change request.", rememberChoice ? "to all" : "single")
            vtWidget.session.executeShowHostWritableStatusLine(true, rememberChoice);
        }
        onNoClicked: {
            const rememberChoice = requestShowHostWritableStatusLine.clickedRememberChoice();
            console.log("[Terminal] Denying font change request.", rememberChoice ? "to all" : "single")
            vtWidget.session.executeShowHostWritableStatusLine(false, rememberChoice);
        }
        onRejected: {
            console.log("[Terminal] Buffer capture request rejected.")
            vtWidget.session.executeShowHostWritableStatusLine(false, false);
        }
    }

    // Callback, to be invoked whenever the GUI scrollbar has been changed.
    // This will update the VT's viewport respectively.
    function onScrollBarPositionChanged() {
        let vt = vtWidget.session;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);

        vt.scrollOffset = vt.historyLineCount - vbar.position * totalLineCount;
    }

    // Callback to be invoked whenever the VT's viewport is changing.
    // This will update the GUI (vertical) scrollbar respectively.
    function updateScrollBarPosition() {
        let vt = vtWidget.session;
        let totalLineCount = (vt.pageLineCount + vt.historyLineCount);

        vbar.position = (vt.historyLineCount - vt.scrollOffset) / totalLineCount;
    }

    onTerminated: {
        console.log("Client process terminated. Closing the window.");
        Window.window.close(); // https://stackoverflow.com/a/53829662/386670
    }

    onSessionChanged: (s) => {
        let vt = vtWidget.session;

        // Connect bell control code with an actual sound effect.
        vt.onBell.connect(bellSoundEffect.play);

        // Link showNotification signal.
        vt.onShowNotification.connect(vtWidget.showNotification);

        // Link opacityChanged signal.
        vt.onOpacityChanged.connect(vtWidget.opacityChanged);

        // Update the VT's viewport whenever the scrollbar's position changes.
        vbar.onPositionChanged.connect(onScrollBarPositionChanged);

        // Update the scrollbar position whenever the scrollbar size changes, because
        // the position is calculated based on scrollbar's size.
        vbar.onSizeChanged.connect(updateScrollBarPosition);

        // Update the scrollbar's position whenever the VT's viewport changes.
        vt.onScrollOffsetChanged.connect(updateScrollBarPosition);

        // Permission-wall related hooks.
        vt.requestPermissionForFontChange.connect(requestFontChangeDialog.open);
        vt.requestPermissionForBufferCapture.connect(requestBufferCaptureDialog.open);
        vt.requestPermissionForShowHostWritableStatusLine.connect(requestShowHostWritableStatusLine.open);
    }
}
