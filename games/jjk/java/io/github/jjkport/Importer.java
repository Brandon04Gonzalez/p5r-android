package io.github.jjkport;

/** The import on the phone (games/jjk/import/jjk_import_jni.cpp): only in ImportActivity's process. */
final class Importer {
    static {
        System.loadLibrary("jjkimport");
    }

    /** Blocking: imports the dump and translates supported ExeFS shaders into dataDir/spirv. */
    static native int run(int nspFd, String nspName, String keysPath, String titleKeysPath, String dataDir, String workDir,
                          int jobs);

    /** {phase, done, total}: 1 opening the game, 2 extracting the shaders, 3 translating them, 4 finishing, 5 done. */
    static native int[] progress();

    static native String error();

    private Importer() {}
}
