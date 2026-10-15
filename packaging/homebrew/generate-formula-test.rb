# frozen_string_literal: true

require "minitest/autorun"
require "tmpdir"

require_relative "generate-formula"

class GenerateFormulaTest < Minitest::Test
  MACOS_SHA256 = "a" * 64
  LICENSE_SHA256 = "d" * 64

  def setup
    @temporary_directory = Dir.mktmpdir("lupe-homebrew-test")
    @checksums_path = File.join(@temporary_directory, "SHA256SUMS")
    File.write(
      @checksums_path,
      <<~CHECKSUMS,
        #{MACOS_SHA256}  lupe-v1.2.3-macos-universal.tar.gz
        #{"b" * 64}  lupe-v1.2.3-linux-x86_64.tar.gz
        #{"c" * 64}  lupe-v1.2.3-windows-x86_64.zip
      CHECKSUMS
    )
  end

  def teardown
    FileUtils.remove_entry(@temporary_directory)
  end

  def test_renders_platform_artifacts_and_license
    formula = LupeHomebrew.render(
      "v1.2.3",
      @checksums_path,
      license_sha256: LICENSE_SHA256,
    )

    assert_includes formula, "lupe-v1.2.3-macos-universal.tar.gz"
    assert_includes formula, "class Lupe < Formula"
    assert_includes formula, 'bin.install "lupe" => "lupe"'
    assert_includes formula, 'assert_match "lupe #{version}"'
    refute_includes formula, "loupe"
    assert_includes formula, %(sha256 "#{MACOS_SHA256}")
    assert_includes formula, %(sha256 "#{LICENSE_SHA256}")
    refute_match(/@[A-Z0-9_]+@/, formula)
  end

  def test_installs_historical_release_under_new_name
    formula = LupeHomebrew.render(
      "v0.3.0",
      File.join(__dir__, "testdata/v0.3.0-SHA256SUMS"),
      license_sha256: LICENSE_SHA256,
    )

    assert_includes formula, "class Lupe < Formula"
    assert_includes formula, "loupe-v0.3.0-macos-universal.tar.gz"
    assert_includes formula, 'bin.install "loupe" => "lupe"'
    assert_includes formula, 'assert_match "loupe #{version}"'
    assert_includes formula, '#{bin}/lupe --version'
    refute_match(/@[A-Z0-9_]+@/, formula)
  end

  def test_rejects_non_stable_tag
    error = assert_raises(ArgumentError) do
      LupeHomebrew.render(
        "v1.2.3-rc.1",
        @checksums_path,
        license_sha256: LICENSE_SHA256,
      )
    end

    assert_match "stable semantic version", error.message
  end

  def test_requires_macos_checksum
    File.write(
      @checksums_path,
      "#{"b" * 64}  lupe-v1.2.3-linux-x86_64.tar.gz\n",
    )

    error = assert_raises(ArgumentError) do
      LupeHomebrew.render(
        "v1.2.3",
        @checksums_path,
        license_sha256: LICENSE_SHA256,
      )
    end

    assert_match "lupe-v1.2.3-macos-universal.tar.gz", error.message
  end
end
