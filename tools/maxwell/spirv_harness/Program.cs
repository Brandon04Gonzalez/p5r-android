// spirv_harness: run Ryujinx's Maxwell shader translator offline on blocks extracted by tools/maxwell/containers.py.
//
//   spirv_harness <code.bin> <ctrl.bin> [-o out.spv] [--glsl]
//       one stage: code block + NVN control section as written by containers.py --extract
//       (--glsl: Vulkan GLSL text instead of SPIR-V, to read what the translator made or compare backends)
//   spirv_harness --batch <extracted dir> --out <dir> [--jobs 2] [--limit N] [--no-val] [--no-retry] [--debug-mode] [--target-env vulkan1.2]
//       every distinct code block named in any manifest.tsv under <extracted dir>; writes <hash>.spv, results.tsv
//       (one line per block) and summary.txt (counts, exceptions and translator messages grouped by text)
//
// Everything this program learns about the block comes from the control section (see docs/nvn/SHADERS.md):
// +0x708 program start (0x30 graphics, 0x100 compute), +0x6fc/+0x700 constant data size/offset. The stage comes
// from the SPH inside the code (graphics) or from the NVN header magic (0x12345679 = compute).

using Ryujinx.Graphics.Shader;
using Ryujinx.Graphics.Shader.Translation;
using System;
using System.Collections.Concurrent;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using System.Threading;
using System.Threading.Tasks;

namespace SpirvHarness
{
    sealed record Job(string Hash, string Group, string CodePath, string CtrlPath, int Stages);

    sealed class Result
    {
        public string Hash, Group, Stage = "?", Status = "?", Error = "", ValError = "";
        public int Stages, SpvBytes;
        public double Ms;
        public List<string> Messages = new();
        public string Resources = "";
        public string Queries = "";
    }

    static class Program
    {
        const uint ComputeMagic = 0x12345679;
        static readonly TimeSpan Timeout = TimeSpan.FromSeconds(60);
        static bool NoRetry;   // --no-retry: report attempt 1 only (the translator as Ryujinx runs it)
        static bool Debug;     // --debug-mode: DebugMode from attempt 1 (also reports undecodable opcodes)

        static int Main(string[] args)
        {
            NoRetry = args.Contains("--no-retry");
            Debug = args.Contains("--debug-mode");
            try
            {
                if (args.Length >= 2 && args[0] == "--batch") return Batch(args);
                if (args.Length >= 2 && !args[0].StartsWith("-")) return Single(args);
            }
            catch (ArgumentException e)
            {
                Console.Error.WriteLine(e.Message);
            }
            Console.Error.WriteLine("usage: spirv_harness <code.bin> <ctrl.bin> [-o out.spv]\n" +
                                    "       spirv_harness --batch <extracted dir> --out <dir> [--jobs N] [--limit N] [--no-val] [--no-retry] [--debug-mode] [--target-env E]");
            return 2;
        }

        static string Opt(string[] args, string name, string def)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : def;
        }

        static int Single(string[] args)
        {
            bool glsl = args.Contains("--glsl");
            string output = Opt(args, "-o", Path.ChangeExtension(args[0], glsl ? ".glsl" : ".spv"));
            Result r = Translate(File.ReadAllBytes(args[0]), File.ReadAllBytes(args[1]), out byte[] spv, glsl);
            if (spv != null) File.WriteAllBytes(output, spv);
            Console.WriteLine($"{r.Stage} {r.Status} {r.SpvBytes} bytes {r.Ms:F1} ms -> {(spv != null ? output : "-")}");
            Console.WriteLine($"resources: {r.Resources}");
            foreach (string m in r.Messages) Console.WriteLine($"translator: {m}");
            if (r.Error != "") Console.WriteLine($"error: {r.Error}");
            return spv != null ? 0 : 1;
        }

        // One code block through the translator. Never throws: failures are recorded in the result.
        static Result Translate(byte[] code, byte[] ctrl, out byte[] spv, bool glsl = false)
        {
            Result r = new();
            spv = null;
            int start = BitConverter.ToInt32(ctrl, 0x708);
            int constSize = BitConverter.ToInt32(ctrl, 0x6fc);
            int constOff = BitConverter.ToInt32(ctrl, 0x700);
            // P5R also has compute blocks with no NVN header at all: program start 0 and a work-group size.
            bool compute = BitConverter.ToUInt32(code, 0) == ComputeMagic ||
                           (start == 0 && BitConverter.ToUInt32(ctrl, 0x748) != 0);
            // SPH word 0 bits 10..13: 1 vertex, 5 pixel (only these exist in Wonder). Used here for binding ranges only;
            // the translator reads the SPH itself.
            int sphType = compute ? 0 : (BitConverter.ToInt32(code, start) >> 10) & 0xf;
            ShaderStage stage = compute ? ShaderStage.Compute : sphType == 5 ? ShaderStage.Fragment : ShaderStage.Vertex;
            r.Stage = stage.ToString().ToLowerInvariant();

            Stopwatch sw = Stopwatch.StartNew();
            // Attempt 1: normal translation. Attempt 2 (only after an exception, SPIR-V only, unless NoRetry):
            // DebugMode, which turns off the translator's expression propagation. That pass folds the temporaries
            // Ryujinx creates for SPIR-V call arguments back into the call, and the SPIR-V backend then throws in
            // GenerateCall (see docs/nvn/TRANSLATOR.md, "Failures"). Status "ok-retry" = translated by attempt 2.
            for (int attempt = 1; attempt <= (glsl || NoRetry ? 1 : 2); attempt++)
            {
                StaticGpuAccessor gpu = new(code, constOff, constSize, stage, ctrl);
                TranslationFlags flags = (compute ? TranslationFlags.Compute : TranslationFlags.None) |
                                         (attempt == 2 || Debug ? TranslationFlags.DebugMode : TranslationFlags.None);
                TranslationOptions options = new(glsl ? TargetLanguage.Glsl : TargetLanguage.Spirv, TargetApi.Vulkan, flags);
                try
                {
                    ShaderProgram program = Translator.CreateContext((ulong)start, gpu, options).Translate();
                    spv = glsl ? Encoding.UTF8.GetBytes(program.Code) : program.BinaryCode;
                    if (!glsl && spv != null && program.Info.Images.Count > 0) spv = AddImageWithoutFormatCapabilities(spv);
                    r.SpvBytes = spv?.Length ?? 0;
                    r.Status = spv == null ? "no-output" : attempt == 1 ? "ok" : "ok-retry";
                    r.Resources = DescribeResources(program.Info);
                }
                catch (Exception e)
                {
                    r.Status = "exception";
                    r.Error = $"{e.GetType().Name}: {e.Message}";
                    string frame = e.StackTrace?.Split('\n').FirstOrDefault(l => l.Contains("Ryujinx.Graphics.Shader"))?.Trim();
                    if (frame != null) r.Error += $" @ {Regex.Replace(frame, @" in (/|[A-Za-z]:\\).*", "")}";
                }
                lock (gpu.Messages) r.Messages = gpu.Messages.Distinct().ToList();
                r.Queries = string.Join(',', gpu.DrawTimeQueries);
                if (r.Status != "exception") break;
            }
            r.Ms = sw.Elapsed.TotalMilliseconds;
            return r;
        }

        // Ryujinx declares storage images without a format when the host can load/store them that way (MoltenVK can:
        // shaderStorageImageRead/WriteWithoutFormat), but never adds the two capabilities SPIR-V requires for it.
        // Insert "OpCapability StorageImageReadWithoutFormat / WriteWithoutFormat" right after the 5-word header;
        // capabilities come first in a module, so that is a valid position.
        static byte[] AddImageWithoutFormatCapabilities(byte[] spv)
        {
            const uint OpCapability = 17, ReadWithoutFormat = 55, WriteWithoutFormat = 56;
            uint[] extra = [(2u << 16) | OpCapability, ReadWithoutFormat, (2u << 16) | OpCapability, WriteWithoutFormat];
            byte[] result = new byte[spv.Length + extra.Length * 4];
            Array.Copy(spv, result, 20);
            Buffer.BlockCopy(extra, 0, result, 20, extra.Length * 4);
            Array.Copy(spv, 20, result, 20 + extra.Length * 4, spv.Length - 20);
            return result;
        }

        // Compact description of what the shader binds, in Maxwell terms and in Vulkan terms:
        //   cb<slot>=s<set>b<binding>            constant buffer c[slot]
        //   sb[c<slot>+0x<off>]=s<set>b<binding> storage buffer whose address the shader loads from c[slot][off]
        //   tex<type>[c<slot>:<handle>]=s<set>b<binding>  texture whose handle is word <handle> of c[slot]
        static string DescribeResources(ShaderProgramInfo info)
        {
            List<string> parts = new();
            foreach (BufferDescriptor b in info.CBuffers) parts.Add($"cb{b.Slot}=s{b.Set}b{b.Binding}");
            foreach (BufferDescriptor b in info.SBuffers) parts.Add($"sb[c{b.SbCbSlot}+0x{b.SbCbOffset:x}]=s{b.Set}b{b.Binding}");
            foreach (TextureDescriptor t in info.Textures)
                parts.Add($"tex{t.Type}[c{t.CbufSlot}:{t.HandleIndex}{(t.ArrayLength > 1 ? $"x{t.ArrayLength}" : "")}]=s{t.Set}b{t.Binding}");
            foreach (TextureDescriptor t in info.Images)
                parts.Add($"img{t.Type}[c{t.CbufSlot}:{t.HandleIndex}{(t.ArrayLength > 1 ? $"x{t.ArrayLength}" : "")}]=s{t.Set}b{t.Binding}");
            return string.Join(' ', parts).Replace(", ", "|");
        }

        // All distinct code blocks under the extraction root (one manifest.tsv per extracted container).
        static List<Job> LoadJobs(string root)
        {
            Dictionary<string, Job> jobs = new();
            foreach (string manifest in Directory.EnumerateFiles(root, "manifest.tsv", SearchOption.AllDirectories)
                                                .OrderBy(x => x.Replace('\\', '/'), StringComparer.Ordinal))  // same order on Windows
            {
                string dir = Path.GetDirectoryName(manifest);
                string group = Path.GetRelativePath(root, dir);
                foreach (string line in File.ReadLines(manifest).Skip(1))
                {
                    string[] f = line.Split('\t');
                    string hash = f[10];
                    if (jobs.TryGetValue(hash, out Job j)) jobs[hash] = j with { Stages = j.Stages + 1 };
                    else jobs[hash] = new Job(hash, group, Path.Combine(dir, f[11]), Path.Combine(dir, f[12]), 1);
                }
            }
            return jobs.Values.ToList();
        }

        static int Batch(string[] args)
        {
            string root = args[1];
            string outDir = Opt(args, "--out", null) ?? throw new ArgumentException("--out is required");
            int jobsN = int.Parse(Opt(args, "--jobs", "2"));
            int limit = int.Parse(Opt(args, "--limit", "0"));
            string targetEnv = Opt(args, "--target-env", "vulkan1.2");
            bool validate = !args.Contains("--no-val");
            string spirvVal = Opt(args, "--spirv-val", "/opt/homebrew/bin/spirv-val");
            Directory.CreateDirectory(outDir);

            List<Job> jobs = LoadJobs(root);
            if (limit > 0) jobs = jobs.Take(limit).ToList();
            Console.WriteLine($"{jobs.Count} distinct code blocks, {jobsN} jobs, validation {(validate ? targetEnv : "off")}");

            ConcurrentBag<Result> results = new();
            int done = 0;
            Stopwatch total = Stopwatch.StartNew();
            Parallel.ForEach(jobs, new ParallelOptions { MaxDegreeOfParallelism = jobsN }, job =>
            {
                Result r;
                byte[] spv = null;
                // A translator bug could loop forever; run on a task so a stuck block is reported, not waited on.
                Task<(Result, byte[])> t = Task.Run(() =>
                {
                    Result x = Translate(File.ReadAllBytes(job.CodePath), File.ReadAllBytes(job.CtrlPath), out byte[] s);
                    return (x, s);
                });
                if (t.Wait(Timeout)) (r, spv) = t.Result;
                else r = new Result { Status = "timeout", Error = $"no result after {Timeout.TotalSeconds} s" };
                r.Hash = job.Hash;
                r.Group = job.Group;
                r.Stages = job.Stages;
                if (spv != null)
                {
                    string spvPath = Path.Combine(outDir, job.Hash + ".spv");
                    File.WriteAllBytes(spvPath, spv);
                    if (validate)
                    {
                        r.ValError = RunSpirvVal(spirvVal, targetEnv, spvPath);
                        if (r.ValError != "") r.Status = "invalid";
                    }
                }
                results.Add(r);
                int n = Interlocked.Increment(ref done);
                if (n % 500 == 0) Console.WriteLine($"  {n}/{jobs.Count}  {total.Elapsed.TotalSeconds:F0} s");
            });

            List<Result> all = results.OrderBy(r => r.Group).ThenBy(r => r.Hash).ToList();
            WriteResults(Path.Combine(outDir, "results.tsv"), all);
            string summary = Summarize(all, total.Elapsed, targetEnv);
            File.WriteAllText(Path.Combine(outDir, "summary.txt"), summary);
            Console.Write(summary);
            return 0;
        }

        // spirv-val prints "error: line N: <message>" then the offending instruction; keep the first line.
        static string RunSpirvVal(string tool, string targetEnv, string path)
        {
            ProcessStartInfo psi = new(tool, ["--target-env", targetEnv, path])
            {
                RedirectStandardError = true,
                RedirectStandardOutput = true,
            };
            using Process p = Process.Start(psi);
            string err = p.StandardError.ReadToEnd() + p.StandardOutput.ReadToEnd();
            p.WaitForExit();
            if (p.ExitCode == 0) return "";
            string first = err.Split('\n').FirstOrDefault(l => l.StartsWith("error")) ?? err.Split('\n')[0];
            return first.Trim();
        }

        static void WriteResults(string path, List<Result> all)
        {
            using StreamWriter w = new(path) { NewLine = "\n" };  // LF on Windows too: same bytes as on macOS
            w.WriteLine("hash\tgroup\tstage\tstages_using\tstatus\tms\tspv_bytes\tresources\ttranslator_messages\terror\tdraw_time_queries");
            foreach (Result r in all)
            {
                string err = (r.Error + " " + r.ValError).Trim().Replace('\t', ' ').Replace('\n', ' ');
                w.WriteLine($"{r.Hash}\t{r.Group}\t{r.Stage}\t{r.Stages}\t{r.Status}\t{r.Ms:F1}\t{r.SpvBytes}\t{r.Resources}\t" +
                            $"{string.Join(" | ", r.Messages).Replace('\t', ' ')}\t{err}\t{r.Queries}");
            }
        }

        // Group messages that differ only in numbers ("Attribute offset 0x3A0 ..." etc.) so the summary stays short.
        static string Normalize(string s) => Regex.Replace(Regex.Replace(s, @"0x[0-9A-Fa-f]+", "0x#"), @"\b\d+\b", "#");

        static string Summarize(List<Result> all, TimeSpan elapsed, string targetEnv)
        {
            StringBuilder sb = new();
            int stagesTotal = all.Sum(r => r.Stages);
            sb.AppendLine($"blocks {all.Count} (stages using them: {stagesTotal}), {elapsed.TotalSeconds:F0} s, spirv-val --target-env {targetEnv}");
            sb.AppendLine();
            sb.AppendLine("status        blocks   stages   (per stage kind)");
            foreach (var g in all.GroupBy(r => r.Status).OrderByDescending(g => g.Count()))
            {
                string kinds = string.Join(", ", g.GroupBy(r => r.Stage).OrderBy(k => k.Key).Select(k => $"{k.Key} {k.Count()}"));
                sb.AppendLine($"{g.Key,-12} {g.Count(),7} {g.Sum(r => r.Stages),8}   ({kinds})");
            }
            sb.AppendLine();
            sb.AppendLine("per group (blocks: ok / ok-retry / invalid / exception / timeout)");
            foreach (var g in all.GroupBy(r => Regex.Replace(r.Group, @"^(effect|layout)_.*", "$1_*")).OrderBy(g => g.Key))
            {
                sb.AppendLine($"  {g.Key,-24} {g.Count(),6}: {g.Count(r => r.Status == "ok")} / {g.Count(r => r.Status == "ok-retry")} / {g.Count(r => r.Status == "invalid")} / " +
                              $"{g.Count(r => r.Status == "exception")} / {g.Count(r => r.Status == "timeout")}");
            }
            void Top(string title, IEnumerable<string> items, int n = 25)
            {
                var groups = items.GroupBy(Normalize).OrderByDescending(g => g.Count()).ToList();
                if (groups.Count == 0) return;
                sb.AppendLine();
                sb.AppendLine(title);
                foreach (var g in groups.Take(n)) sb.AppendLine($"  {g.Count(),6}  {g.First()}");
                if (groups.Count > n) sb.AppendLine($"  ... {groups.Count - n} more kinds");
            }
            Top("exceptions (by message)", all.Where(r => r.Status is "exception" or "timeout").Select(r => r.Error));
            Top("spirv-val errors (first error, by message)", all.Where(r => r.ValError != "").Select(r => r.ValError));
            Top("translator messages (blocks reporting them)", all.SelectMany(r => r.Messages));
            Top("draw-time queries (blocks making them)", all.SelectMany(r => r.Queries.Split(',', StringSplitOptions.RemoveEmptyEntries)));
            Top("resource kinds (blocks using them)", all.SelectMany(r => r.Resources.Split(' ', StringSplitOptions.RemoveEmptyEntries)
                .Select(x => Regex.Replace(x, @"=s\d+b\d+$", "")).Distinct()), 60);
            return sb.ToString();
        }
    }
}
