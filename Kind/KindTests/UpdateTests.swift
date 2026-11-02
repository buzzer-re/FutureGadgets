import Foundation
import Testing
@testable import Kind

@Suite("Updates")
struct UpdateTests {
    @Test func parsesVersionsAndTags() {
        #expect(AppVersion("1.2.3")?.components == [1, 2, 3])
        #expect(AppVersion("v0.1.0")?.components == [0, 1, 0])
        #expect(AppVersion("V2")?.components == [2])
        for invalid in ["", "v", "1..2", "1.2.", "1.2.3-beta", "+1.2", "1.2.3.4.5", "one"] {
            #expect(AppVersion(invalid) == nil, "\(invalid)")
        }
    }

    @Test func comparesNumerically() throws {
        func v(_ string: String) throws -> AppVersion { try #require(AppVersion(string)) }
        #expect(try v("1.10.0") > v("1.9.9"))
        #expect(try v("0.1.0") < v("1.0"))
        #expect(try v("2") > v("1.99"))
        #expect(try v("1.2") == v("1.2.0"))
        #expect(try Set([v("1.2"), v("1.2.0.0")]).count == 1)
        #expect(try !(v("1.2.0") < v("1.2")))
    }

    @Test func decodesLatestReleaseResponse() throws {
        let json = """
        {
          "url": "https://api.github.com/repos/example/Kind/releases/1",
          "html_url": "https://github.com/example/Kind/releases/tag/v1.2.0",
          "tag_name": "v1.2.0",
          "name": "Kind 1.2.0",
          "draft": false,
          "prerelease": false,
          "published_at": "2026-10-01T12:00:00Z",
          "body": "## Installing\\n\\n* Drag **Kind** to Applications",
          "assets": [
            { "name": "Kind-1.2.0.dmg.sha256", "size": 81, "content_type": "application/octet-stream",
              "browser_download_url": "https://github.com/example/Kind/releases/download/v1.2.0/Kind-1.2.0.dmg.sha256" },
            { "name": "Kind-1.2.0.dmg", "size": 8400000, "content_type": "application/x-apple-diskimage",
              "browser_download_url": "https://github.com/example/Kind/releases/download/v1.2.0/Kind-1.2.0.dmg" }
          ]
        }
        """
        let release = try Release.decode(Data(json.utf8))
        #expect(release.version == AppVersion("1.2.0"))
        #expect(release.displayVersion == "1.2.0")
        #expect(release.pageURL.absoluteString.hasSuffix("/tag/v1.2.0"))
        #expect(release.publishedAt == Date(timeIntervalSince1970: 1_790_856_000))
        #expect(release.diskImage?.name == "Kind-1.2.0.dmg")
        #expect(release.diskImage?.size == 8_400_000)
        #expect(release.diskImageChecksum?.name == "Kind-1.2.0.dmg.sha256")
    }

    @Test func releaseWithoutAssetsHasNoDiskImage() throws {
        let json = #"{"tag_name":"v2.0","name":null,"body":null,"html_url":"https://github.com/example/Kind","published_at":null,"assets":[]}"#
        let release = try Release.decode(Data(json.utf8))
        #expect(release.diskImage == nil)
        #expect(release.diskImageChecksum == nil)
    }

    @Test func parsesShasumOutput() {
        let digest = String(repeating: "aB", count: 32)
        #expect(Release.parseChecksum("\(digest)  Kind-1.2.0.dmg\n") == digest.lowercased())
        #expect(Release.parseChecksum("not-a-digest  Kind.dmg") == nil)
        #expect(Release.parseChecksum("") == nil)
    }

    @Test func rendersReleaseNotesBlocks() {
        let rendered = String(ReleaseNotes.render("## Installing\n<!-- hidden -->\n* Drag **Kind**\n- Open it").characters)
        #expect(rendered == "Installing\n•  Drag Kind\n•  Open it")
        #expect(String(ReleaseNotes.render(nil).characters) == "No release notes.")
    }
}
