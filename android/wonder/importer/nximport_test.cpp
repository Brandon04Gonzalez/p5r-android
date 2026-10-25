// Host test of the XCI/NSP container parsing: builds tiny synthetic HFS0/PFS0 images (no game data, no real keys) and
// checks what the importer finds in them. The synthetic files are not valid NCAs, so every run ends in the importer's
// "no game program" error; what is asserted is the log line that says what it found and the errors of broken images.
//   nximport_test
#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nximport.h"

namespace {

typedef std::vector<uint8_t> Bytes;
struct F {
    std::string name;
    Bytes data;
};

void put32(Bytes& b, size_t at, uint32_t v) {
    for (int i = 0; i < 4; i++) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}
void put64(Bytes& b, size_t at, uint64_t v) {
    for (int i = 0; i < 8; i++) b[at + size_t(i)] = uint8_t(v >> (8 * i));
}

// PFS0 (esz 0x18) or HFS0 (esz 0x40) image of the files.
Bytes build(const std::vector<F>& files, bool hfs) {
    const size_t esz = hfs ? 0x40 : 0x18;
    std::string names;
    std::vector<uint32_t> name_off;
    for (auto& f : files) name_off.push_back(uint32_t(names.size())), names += f.name + '\0';
    while ((names.size() & 0x1F) != 0) names += '\0';
    Bytes b(16 + files.size() * esz + names.size());
    memcpy(b.data(), hfs ? "HFS0" : "PFS0", 4);
    put32(b, 4, uint32_t(files.size()));
    put32(b, 8, uint32_t(names.size()));
    uint64_t off = 0;
    for (size_t i = 0; i < files.size(); i++) {
        size_t e = 16 + i * esz;
        put64(b, e, off);
        put64(b, e + 8, files[i].data.size());
        put32(b, e + 16, name_off[i]);
        if (hfs) put32(b, e + 20, 0x200);  // hashed size (unused)
        off += files[i].data.size();
    }
    memcpy(b.data() + 16 + files.size() * esz, names.data(), names.size());
    for (auto& f : files) b.insert(b.end(), f.data.begin(), f.data.end());
    return b;
}

// XCI: 0x1000 bytes of header ("HEAD" at 0x100, root HFS0 offset 0x130 / header size 0x138), then the root HFS0 of the partitions.
Bytes build_xci(const std::vector<F>& parts, bool with_head = true) {
    std::vector<F> root;
    for (auto& p : parts) root.push_back({p.name, p.data});
    Bytes r = build(root, true);
    Bytes x(0x1000, 0);
    if (with_head) memcpy(x.data() + 0x100, "HEAD", 4);
    put64(x, 0x130, 0x1000);
    put64(x, 0x138, 16 + root.size() * 0x40 + 0x40);
    x.insert(x.end(), r.begin(), r.end());
    return x;
}

std::string tmpdir;
int fails = 0;

std::string write_file(const std::string& name, const Bytes& b) {
    std::string p = tmpdir + "/" + name;
    FILE* f = fopen(p.c_str(), "wb");
    if (!f || (!b.empty() && fwrite(b.data(), 1, b.size(), f) != b.size())) {
        fprintf(stderr, "cannot write %s\n", p.c_str());
        exit(2);
    }
    fclose(f);
    return p;
}

const char* kKeys =
    "header_key = 00000000000000000000000000000000"
    "00000000000000000000000000000000\n";

struct Result {
    int rc;
    std::string error, log;
};

Result run(const std::vector<std::pair<std::string, Bytes>>& files) {
    std::vector<nxi::InputFile> in;
    std::vector<std::string> paths;
    Bytes keys(kKeys, kKeys + strlen(kKeys));
    paths.push_back(write_file("prod.keys", keys));
    in.push_back({"prod.keys", -1});
    for (auto& [n, b] : files) {
        paths.push_back(write_file(n, b));
        in.push_back({n, -1});
    }
    for (size_t i = 0; i < in.size(); i++) in[i].fd = open(paths[i].c_str(), O_RDONLY | O_CLOEXEC);
    Result r;
    nxi::Options opt;
    opt.out_dir = tmpdir + "/out";
    opt.on_log = [&](const std::string& s) { r.log += s + "\n"; };
    r.rc = nxi::run_import(in, opt, r.error);
    for (auto& i : in) close(i.fd);
    return r;
}

void check(bool ok, const char* what, const Result& r) {
    printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) {
        fails++;
        printf("     rc=%d error=[%s]\n%s", r.rc, r.error.c_str(), r.log.c_str());
    }
}
bool has(const std::string& s, const char* t) { return s.find(t) != std::string::npos; }

}  // namespace

int main() {
    char tpl[] = "/tmp/nximport_test_XXXXXX";
    if (!mkdtemp(tpl)) return 2;
    tmpdir = tpl;
    Bytes junk(0x40, 0xAB);

    std::vector<F> parts = {{"update", build({{"upd0.nca", junk}}, true)},
                            {"normal", build({}, true)},
                            {"secure", build({{"a.nca", junk}, {"b.cnmt.nca", junk}, {"x.tik", junk}}, true)},
                            {"logo", build({{"logo.png", junk}}, true)}};

    Bytes xci = build_xci(parts);
    Result r = run({{"game.xci", xci}});
    check(r.rc != 0 && has(r.error, "no game program"), "XCI parses and ends at 'no game program' (synthetic NCAs)", r);
    check(has(r.log, "partitions update, normal, secure, logo; 3 files in secure/normal"),
          "XCI: root partitions listed, only secure+normal files used (update/logo skipped)", r);

    r = run({{"game.xci", build_xci(parts, false)}});
    check(r.rc != 0 && has(r.error, "not an XCI"), "XCI without HEAD is rejected", r);

    Bytes cut(xci.begin(), xci.begin() + long(xci.size() - 8));
    r = run({{"game.xci", cut}});
    check(r.rc != 0 && has(r.error, "truncated"), "truncated XCI is rejected", r);

    r = run({{"game.xci", build_xci({{"update", build({{"u.nca", junk}}, true)}})}});
    check(r.rc != 0 && has(r.error, "no secure partition"), "XCI without a secure partition is rejected", r);

    // split in two parts, cut in the middle of the root HFS0
    size_t half = 0x1000 + 100;
    Bytes p0(xci.begin(), xci.begin() + long(half)), p1(xci.begin() + long(half), xci.end());
    r = run({{"game.xc0", p0}, {"game.xc1", p1}});
    check(r.rc != 0 && has(r.error, "no game program") && has(r.log, "2 parts joined"), "split XCI (.xc0/.xc1) is joined", r);
    r = run({{"game.xci.00", p0}, {"game.xci.01", p1}});
    check(r.rc != 0 && has(r.error, "no game program") && has(r.log, "2 parts joined"), "split XCI (.xci.00/.xci.01) is joined", r);
    r = run({{"game.xc1", p1}});
    check(r.rc != 0 && has(r.error, "first part"), "split XCI without its first part is reported", r);
    r = run({{"game.xc0", p0}, {"game.xc2", p1}});
    check(r.rc != 0 && has(r.error, "part of the split XCI is missing"), "split XCI with a gap is reported", r);

    // NSP still works (PFS0), alone and mixed with an XCI
    Bytes nsp = build({{"a.nca", junk}, {"t.tik", junk}}, false);
    r = run({{"game.nsp", nsp}});
    check(r.rc != 0 && has(r.error, "no game program"), "NSP (PFS0) parses", r);
    r = run({{"game.nsp", nsp}, {"game.xci", xci}});
    check(r.rc != 0 && has(r.error, "no game program") && has(r.log, "gamecard image"), "NSP + XCI in the same folder", r);
    r = run({{"fake.nsp", xci}});
    check(r.rc != 0 && has(r.error, "not an NSP"), "an XCI named .nsp is rejected", r);
    r = run({});
    check(r.rc != 0 && has(r.error, "no .nsp or .xci"), "no container in the folder", r);

    printf("%s\n", fails ? "FAILED" : "all passed");
    std::string cmd = "rm -rf '" + tmpdir + "'";
    if (system(cmd.c_str())) {}
    return fails ? 1 : 0;
}
