import XCTest

final class KindUITests: XCTestCase {
    override func setUpWithError() throws {
        continueAfterFailure = false
    }

    /// UI tests must never talk to a Kindle that happens to be plugged in.
    private func makeApp() -> XCUIApplication {
        let app = XCUIApplication()
        app.launchArguments += ["-KindDisableDevices", "-KindDisableUpdateCheck"]
        return app
    }

    @MainActor
    func testShowsConnectPromptWithoutAKindle() throws {
        let app = makeApp()
        app.launch()
        XCTAssertTrue(app.staticTexts["Connect your Kindle"].waitForExistence(timeout: 5))
    }

    @MainActor
    func testLaunchPerformance() throws {
        measure(metrics: [XCTApplicationLaunchMetric()]) {
            makeApp().launch()
        }
    }
}
