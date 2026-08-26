// abi_profile_test.go — asserts src/common/nvkvm_abi.h against MEASURED values.
//
// nvkvm_abi.h claims its rows were measured by compiling sizeof/offsetof
// against open-gpu-kernel-modules.  Until this file existed, nothing checked
// that claim: the header said "tests/abi_parity asserts the table" while the
// only tests here were struct-size parity checks that never touched the profile
// table.  A wrong row is silent (a truncated forward, or an embedded-fd fixup
// ~8 KiB past the real field), so the claim needs teeth.
//
// The table below is the raw output of tools/abi_derive.sh — one line per OGKM
// tag whose probes were actually compiled.  Regenerate with:
//
//	tools/abi_derive.sh --format csv
//
// Every line is a measurement at a named tag.  Do NOT add a line for a tag you
// have not measured, and do NOT interpolate one branch from its neighbour: the
// whole point is that the version->layout map has boundaries you cannot guess
// (535 and 550 each shift *within* the branch).
//
// RUN WITH -count=1.  Go's *build* cache does track the cgo-included header, so
// an edit to nvkvm_abi.h is picked up — but the *test-result* cache is keyed on
// the test binary alone, so a plain `go test` after editing the header can
// replay a stale PASS.  Verified: breaking the 610 bucket and re-running
// `go test` reported ok; the same header with `go test -count=1` failed.

package abi_parity

import "testing"

// measured is the ABI matrix as compiled from OGKM headers.
// Fields, in order:
//
//	uvm_map_ext_size, uvm_map_ext_fd_off, uvm_sem_pool_size,
//	chan_alloc_size, vaspace_alloc_size, mem_alloc_size,
//	nv00de_alloc_size, nvos46_size, nvos46_status_off
//
// nv00de_alloc_size is 0 for 515/520 only, where NV00DE / RM_USER_SHARED_DATA
// does not exist anywhere in the tree — "absent", not "measured as zero".
var measured = []struct {
	tag  string
	want [9]uint32
}{
	// --- 515/520: NV00DE class absent; chan measured via the pre-rename
	//     NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS spelling in nvos.h.
	{"515.43.04", [9]uint32{1200, 1184, 1184, 304, 48, 120, 0, 56, 48}},
	{"515.57", [9]uint32{1200, 1184, 1184, 304, 48, 120, 0, 56, 48}},
	{"515.105.01", [9]uint32{1200, 1184, 1184, 304, 48, 120, 0, 56, 48}},
	{"520.56.06", [9]uint32{1200, 1184, 1184, 304, 48, 120, 0, 56, 48}},
	{"520.61.07", [9]uint32{1200, 1184, 1184, 304, 48, 120, 0, 56, 48}},

	// --- 525/530 and the two earliest 535 releases: pre-CC channel (304).
	{"525.47.04", [9]uint32{1200, 1184, 1184, 304, 48, 120, 4, 56, 48}},
	{"525.85.05", [9]uint32{1200, 1184, 1184, 304, 48, 120, 4, 56, 48}},
	{"525.147.05", [9]uint32{1200, 1184, 1184, 304, 48, 120, 4, 56, 48}},
	{"530.30.02", [9]uint32{1200, 1184, 1184, 304, 48, 120, 4, 56, 48}},
	{"530.41.03", [9]uint32{1200, 1184, 1184, 304, 48, 120, 4, 56, 48}},

	// --- INTRA-BRANCH BOUNDARY inside 535: the Confidential-Computing
	//     channel fields (+56 B).  535.54.03 is the original 535 GA and is on
	//     the OLD side; the 535.43.x train crosses over at 535.43.08, which is
	//     newer in wall-clock time than 535.54.03 despite sorting older.
	{"535.43.02", [9]uint32{1200, 1184, 1184, 304, 48, 120, 4, 56, 48}},
	{"535.54.03", [9]uint32{1200, 1184, 1184, 304, 48, 120, 4, 56, 48}},
	{"535.43.08", [9]uint32{1200, 1184, 1184, 360, 48, 120, 4, 56, 48}},
	{"535.43.28", [9]uint32{1200, 1184, 1184, 360, 48, 120, 4, 56, 48}},
	{"535.86.05", [9]uint32{1200, 1184, 1184, 360, 48, 120, 4, 56, 48}},
	{"535.129.03", [9]uint32{1200, 1184, 1184, 360, 48, 120, 4, 56, 48}},
	{"535.183.01", [9]uint32{1200, 1184, 1184, 360, 48, 120, 4, 56, 48}},
	{"535.309.01", [9]uint32{1200, 1184, 1184, 360, 48, 120, 4, 56, 48}},

	// --- 545 is its own layout: V545 mem/nv00de but pre-V550 UVM.  The old
	//     table had no such row and bucketed 545 into 535 (mem 120, nv00de 4).
	{"545.23.06", [9]uint32{1200, 1184, 1184, 360, 48, 128, 8, 56, 48}},
	{"545.29.06", [9]uint32{1200, 1184, 1184, 360, 48, 128, 8, 56, 48}},

	// --- INTRA-BRANCH BOUNDARY inside 550: the V550 UVM per-GPU attribute
	//     array (1200 -> 9264).  550.40.07 is on the OLD side.
	{"550.40.07", [9]uint32{1200, 1184, 1184, 360, 48, 128, 8, 56, 48}},
	{"550.40.53", [9]uint32{9264, 9248, 9248, 360, 48, 128, 8, 56, 48}},
	{"550.54.14", [9]uint32{9264, 9248, 9248, 360, 48, 128, 8, 56, 48}},
	{"550.163.01", [9]uint32{9264, 9248, 9248, 360, 48, 128, 8, 56, 48}},

	{"555.58.02", [9]uint32{9264, 9248, 9248, 360, 48, 128, 8, 56, 48}},
	{"560.35.03", [9]uint32{9264, 9248, 9248, 360, 48, 128, 8, 56, 48}},
	{"565.57.01", [9]uint32{9264, 9248, 9248, 360, 48, 128, 8, 56, 48}},
	{"565.77", [9]uint32{9264, 9248, 9248, 360, 48, 128, 8, 56, 48}},

	// --- 570 adds tpcConfigID to the channel struct (360 -> 368); 575 shares it.
	{"570.86.15", [9]uint32{9264, 9248, 9248, 368, 48, 128, 8, 56, 48}},
	{"570.172.08", [9]uint32{9264, 9248, 9248, 368, 48, 128, 8, 56, 48}},
	{"570.211.01", [9]uint32{9264, 9248, 9248, 368, 48, 128, 8, 56, 48}},
	{"575.51.02", [9]uint32{9264, 9248, 9248, 368, 48, 128, 8, 56, 48}},
	{"575.64.05", [9]uint32{9264, 9248, 9248, 368, 48, 128, 8, 56, 48}},

	// --- 580 grows VASPACE (+Pasid) and NVOS46 (+Flags2,KindOverride).
	//     590 and 595 are MEASURED members of the same layout, not assumed.
	{"580.65.06", [9]uint32{9264, 9248, 9248, 368, 56, 128, 8, 64, 56}},
	{"580.95.05", [9]uint32{9264, 9248, 9248, 368, 56, 128, 8, 64, 56}},
	{"580.178.04", [9]uint32{9264, 9248, 9248, 368, 56, 128, 8, 64, 56}},
	{"590.44.01", [9]uint32{9264, 9248, 9248, 368, 56, 128, 8, 64, 56}},
	{"590.48.01", [9]uint32{9264, 9248, 9248, 368, 56, 128, 8, 64, 56}},
	{"595.44.02", [9]uint32{9264, 9248, 9248, 368, 56, 128, 8, 64, 56}},
	{"595.91.07", [9]uint32{9264, 9248, 9248, 368, 56, 128, 8, 64, 56}},

	// --- 610 adds hHandleVASpace to the channel struct (368 -> 376).  The old
	//     `major >= 580` bucket handed these hosts 368.
	{"610.43.02", [9]uint32{9264, 9248, 9248, 376, 56, 128, 8, 64, 56}},
	{"610.43.03", [9]uint32{9264, 9248, 9248, 376, 56, 128, 8, 64, 56}},
	{"610.57.04", [9]uint32{9264, 9248, 9248, 376, 56, 128, 8, 64, 56}},
}

var fieldNames = [9]string{
	"uvm_map_ext_size", "uvm_map_ext_fd_off", "uvm_sem_pool_size",
	"chan_alloc_size", "vaspace_alloc_size", "mem_alloc_size",
	"nv00de_alloc_size", "nvos46_size", "nvos46_status_off",
}

// TestAbiProfileMatchesMeasured drives nvkvm_abi_for_version() with each
// measured driver version string and compares all nine fields.  This catches
// both a wrong row VALUE and a wrong BUCKET BOUNDARY -- the latter being the
// failure that put 545, 550.40.07, 535.54.03 and 610 on the wrong profile.
func TestAbiProfileMatchesMeasured(t *testing.T) {
	for _, m := range measured {
		got := ProfileForVersion(m.tag)
		for i, name := range fieldNames {
			if got[i] != m.want[i] {
				t.Errorf("%s: %s = %d, measured %d (profile id %d)",
					m.tag, name, got[i], m.want[i], ProfileIDForVersion(m.tag))
			}
		}
	}
}

// TestAbiProfileBoundariesAreSharp asserts that each adjacent pair of OGKM tags
// that straddles a measured boundary really does select DIFFERENT profiles.
// Without this, a selector that collapsed everything to one row would still
// pass the table test above if the rows happened to agree.
func TestAbiProfileBoundariesAreSharp(t *testing.T) {
	boundaries := []struct{ older, newer string }{
		{"520.61.07", "525.47.04"},   // NV00DE class appears
		{"535.54.03", "535.86.05"},   // CC channel fields, INTRA-BRANCH
		{"535.43.02", "535.43.08"},   // same change on the .43 train
		{"535.309.01", "545.23.06"},  // V545 mem/nv00de
		{"550.40.07", "550.40.53"},   // V550 UVM array, INTRA-BRANCH
		{"565.77", "570.86.15"},      // tpcConfigID
		{"575.64.05", "580.65.06"},   // VASPACE +Pasid, NVOS46 +8
		{"595.91.07", "610.43.02"},   // hHandleVASpace
	}
	for _, b := range boundaries {
		o, n := ProfileIDForVersion(b.older), ProfileIDForVersion(b.newer)
		if o == n {
			t.Errorf("%s and %s both select profile %d, but their probes disagree",
				b.older, b.newer, o)
		}
	}
}

// TestAbiProfileSameSideStaysPut is the converse: tags measured to have the
// same layout must land on the same profile.  A selector that over-split would
// still pass the two tests above.
func TestAbiProfileSameSideStaysPut(t *testing.T) {
	same := []struct{ a, b string }{
		{"535.54.03", "525.147.05"}, // 535 GA shares the 525 layout
		{"535.43.28", "535.309.01"},
		{"550.40.07", "545.29.06"},  // 550.40.07 shares the 545 layout
		{"570.211.01", "575.64.05"},
		{"580.178.04", "595.91.07"},
	}
	for _, s := range same {
		if x, y := ProfileIDForVersion(s.a), ProfileIDForVersion(s.b); x != y {
			t.Errorf("%s (profile %d) and %s (profile %d) were measured identical "+
				"but select different profiles", s.a, x, s.b, y)
		}
	}
}

// TestManagedFallbackCommonPrefixFitsEveryMeasuredProfile guards the exact
// compatibility contract used by nvkvm_uvm_ext_mmap(). The fallback only
// fills fields through NV_MEMORY_ALLOCATION_PARAMS.size; requiring the newest
// full struct would reject the measured, shorter pre-545 layouts even though
// every field used is present.
func TestManagedFallbackCommonPrefixFitsEveryMeasuredProfile(t *testing.T) {
	common := uint32(Sizes.NvMemAllocCommon)
	full := uint32(Sizes.NvMemAlloc)
	seenShorter := false

	for _, m := range measured {
		memSize := ProfileForVersion(m.tag)[5]
		if memSize < common {
			t.Errorf("%s: measured mem_alloc_size %d ends before used common prefix %d",
				m.tag, memSize, common)
		}
		if memSize < full {
			seenShorter = true
		}
	}
	if !seenShorter {
		t.Fatal("matrix no longer exercises a measured pre-545 shorter allocation layout")
	}
}
