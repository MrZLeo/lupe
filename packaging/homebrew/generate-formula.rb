#!/usr/bin/env ruby
# frozen_string_literal: true

require "digest"
require "fileutils"
require "open3"
require "pathname"
require "tempfile"

module LupeHomebrew
  ROOT = Pathname(__dir__).join("../..").expand_path
  TEMPLATE = Pathname(__dir__).join("lupe.rb.in")
  TAG_PATTERN = /\Av(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\z/
  CHECKSUM_PATTERN = /\A(?<sha256>[0-9a-fA-F]{64})[ \t]+\*?(?<filename>\S+)\z/

  module_function

  def render(tag, checksums_path, template_path: TEMPLATE, license_sha256: nil)
    unless TAG_PATTERN.match?(tag)
      raise ArgumentError, "tag must be a stable semantic version such as v1.2.3"
    end

    license_sha256 ||= tagged_license_sha256(tag)
    checksums = parse_checksums(checksums_path)
    # Historical releases keep their original archive and executable names.
    archive = "lupe-#{tag}-macos-universal.tar.gz"
    legacy_archive = "loupe-#{tag}-macos-universal.tar.gz"
    binary = !checksums.key?(archive) && checksums.key?(legacy_archive) ? "loupe" : "lupe"
    replacements = {
      "@TAG@" => tag,
      "@BINARY@" => binary,
      "@MACOS_SHA256@" => fetch_checksum(checksums, "#{binary}-#{tag}-macos-universal.tar.gz"),
      "@LICENSE_SHA256@" => license_sha256,
    }

    formula = File.read(template_path)
    replacements.each { |placeholder, value| formula.gsub!(placeholder, value) }

    unresolved = formula.scan(/@[A-Z0-9_]+@/).uniq
    unless unresolved.empty?
      raise ArgumentError, "unresolved template placeholders: #{unresolved.join(", ")}"
    end

    formula
  end

  def write(tag, checksums_path, output_path)
    output = Pathname(output_path)
    FileUtils.mkdir_p(output.dirname)

    Tempfile.create([".lupe", ".rb"], output.dirname) do |file|
      file.write(render(tag, checksums_path))
      file.flush
      FileUtils.chmod(0o644, file.path)
      FileUtils.mv(file.path, output)
    end
  end

  def parse_checksums(path)
    File.readlines(path, chomp: true).each_with_object({}) do |line, checksums|
      next if line.strip.empty?

      match = CHECKSUM_PATTERN.match(line)
      raise ArgumentError, "invalid checksum line: #{line.inspect}" unless match

      filename = match[:filename]
      raise ArgumentError, "duplicate checksum for #{filename}" if checksums.key?(filename)

      checksums[filename] = match[:sha256].downcase
    end
  end

  def fetch_checksum(checksums, filename)
    checksums.fetch(filename) do
      raise ArgumentError, "missing checksum for #{filename}"
    end
  end

  def tagged_license_sha256(tag)
    contents, error, status = Open3.capture3(
      "git",
      "-C",
      ROOT.to_s,
      "show",
      "#{tag}:LICENSE",
    )
    unless status.success?
      raise ArgumentError, "cannot read LICENSE from #{tag}: #{error.strip}"
    end

    Digest::SHA256.hexdigest(contents)
  end
end

if $PROGRAM_NAME == __FILE__
  unless ARGV.length == 3
    warn "usage: #{File.basename($PROGRAM_NAME)} TAG SHA256SUMS OUTPUT"
    exit 2
  end

  LupeHomebrew.write(*ARGV)
end
