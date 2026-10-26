// The harness as a native library (NativeAOT, -p:NativeLib=Shared): the Android app translates the player's shaders on
// the phone with it (games/p5r: the import). Same batch as the command line; the app reads the progress while it runs.
//   int spirv_harness_main(int argc, const char** argv)   the command line's arguments (no program name), 0 = done
//   void spirv_harness_progress(int* done, int* total)    blocks translated so far, and how many there are

using System;
using System.Runtime.InteropServices;
using System.Threading;

namespace SpirvHarness
{
    static unsafe class Exports
    {
        [UnmanagedCallersOnly(EntryPoint = "spirv_harness_main")]
        static int RunExport(int argc, byte** argv)
        {
            string[] args = new string[argc];
            for (int i = 0; i < argc; i++) args[i] = Marshal.PtrToStringUTF8((IntPtr)argv[i]);
            try
            {
                return Program.Run(args);
            }
            catch (Exception e)
            {
                Console.Error.WriteLine(e);
                return 3;
            }
        }

        [UnmanagedCallersOnly(EntryPoint = "spirv_harness_progress")]
        static void ProgressExport(int* done, int* total)
        {
            *done = Volatile.Read(ref Program.Done);
            *total = Volatile.Read(ref Program.Total);
        }
    }
}
