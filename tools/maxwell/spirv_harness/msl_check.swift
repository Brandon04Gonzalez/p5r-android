// Compile Metal Shading Language files with the system Metal compiler (the runtime one MoltenVK itself uses; no
// Xcode needed). Spot check for the MoltenVK path: SPIR-V -> spirv-cross --msl -> this. See docs/nvn/TRANSLATOR.md.
//   swiftc -O tools/maxwell/spirv_harness/msl_check.swift -o private/build/spirv_harness/msl_check
//   private/build/spirv_harness/msl_check <file.metal>...    (no arguments: one path per line on stdin)
// (Build it: the `swift` script interpreter gets no Metal device.)
// Prints "ok <file>" or "fail <file>: <first error line>" per file.
import Foundation
import Metal

guard let device = MTLCreateSystemDefaultDevice() ?? MTLCopyAllDevices().first else { print("no Metal device"); exit(2) }
let options = MTLCompileOptions()
options.languageVersion = .version3_0
var paths = Array(CommandLine.arguments.dropFirst())
if paths.isEmpty { while let line = readLine() { if !line.isEmpty { paths.append(line) } } }
for path in paths {
    guard let source = try? String(contentsOfFile: path, encoding: .utf8) else { print("fail \(path): unreadable"); continue }
    do {
        _ = try device.makeLibrary(source: source, options: options)
        print("ok \(path)")
    } catch {
        let first = "\(error)".split(separator: "\n").first { $0.contains("error:") } ?? Substring("\(error)")
        print("fail \(path): \(first.replacingOccurrences(of: path, with: ""))")
    }
}
