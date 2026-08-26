// ogkm_fixture_test.go — hold nvkvm's UVM_REGISTER_GPU wire constants against a
// COMMITTED measurement of NVIDIA's own headers.
//
// WHY THIS FILE EXISTS.
//
// The rest of this package measures src/abi/*.h with cgo and compares the
// result to a literal written in the test.  For most structs that is fine: the
// literal came from gVisor or from a hand-read of nvos.h and is an independent
// second opinion.  For UVM_REGISTER_GPU it was NOT fine.  The commit that grew
// the struct from 32 to 40 bytes edited src/abi/uvm.h and, in the same commit,
// changed the literal in this package from 32 to 40.  sizes.go includes the
// very header that changed, so the assertion had become
//
//     sizeof(our struct) == the number we just wrote next to our struct
//
// which is self-consistency, not conformance.  It would have passed just as
// green had 40 been wrong.
//
// The load-bearing claim -- "REGISTER_GPU is 40 bytes at every supported OGKM
// tag" -- is a claim about NVIDIA's headers, so the test has to compare against
// NVIDIA's headers.  ogkm_register_gpu.tsv is the output of
// tools/abi_derive.sh --all-published-supported: one row per published OGKM
// tag, each cell produced by compiling and running an offsetof/sizeof probe
// against that tag's checkout.  Its provenance header records when it was
// generated, by what command, and against which tag list.  Committing it is the
// point: the evidence now survives a clone, and regenerating it is a reviewable
// diff rather than a claim in a comment.
//
// To refresh:  tools/abi_derive.sh --all-published-supported --format tsv
// and copy the tag / uvm_register_gpu_* columns here, keeping the header.

package abi_parity

import (
	_ "embed"
	"fmt"
	"sort"
	"strconv"
	"strings"
	"testing"
)

//go:embed ogkm_register_gpu.tsv
var ogkmRegisterGPUFixture string

// ogkmRow is one measured tag: the size of UVM_REGISTER_GPU_PARAMS and the
// offsets of its four named members, as compiled at that tag.
type ogkmRow struct {
	tag        string
	line       int
	size       uint32
	fdOff      uint32
	hClientOff uint32
	hSmcOff    uint32
	statusOff  uint32
}

// layout is the part of a row that must be invariant across tags.
func (r ogkmRow) layout() string {
	return fmt.Sprintf("size=%d rmCtrlFd=%d hClient=%d hSmcPartRef=%d rmStatus=%d",
		r.size, r.fdOff, r.hClientOff, r.hSmcOff, r.statusOff)
}

// wantColumns is the exact column order the parser relies on.  Checking it
// means a regenerated fixture with reordered or renamed columns fails loudly
// here instead of silently comparing the wrong numbers.
var wantColumns = []string{
	"tag",
	"uvm_register_gpu_size",
	"uvm_register_gpu_fd_off",
	"uvm_register_gpu_hclient_off",
	"uvm_register_gpu_hsmcpart_off",
	"uvm_register_gpu_status_off",
}

// loadOGKMFixture parses the embedded TSV.  A cell that the sweep could not
// measure is spelled MISSING or CLONE_FAIL and is a hard error here: an
// unmeasured tag must never read as agreement.
func loadOGKMFixture(t *testing.T) ([]ogkmRow, map[string]string) {
	t.Helper()

	meta := map[string]string{}
	var rows []ogkmRow
	sawHeader := false

	for i, raw := range strings.Split(ogkmRegisterGPUFixture, "\n") {
		lineNo := i + 1
		line := strings.TrimRight(raw, "\r")
		if strings.TrimSpace(line) == "" {
			continue
		}
		if strings.HasPrefix(line, "#") {
			// Provenance header: "# key: value" lines are captured so a test
			// can assert the fixture covers what it says it covers.
			body := strings.TrimSpace(strings.TrimPrefix(line, "#"))
			if k, v, ok := strings.Cut(body, ":"); ok {
				meta[strings.TrimSpace(k)] = strings.TrimSpace(v)
			}
			continue
		}

		fields := strings.Split(line, "\t")
		if !sawHeader {
			sawHeader = true
			if len(fields) != len(wantColumns) {
				t.Fatalf("fixture line %d: %d columns, want %d (%v)",
					lineNo, len(fields), len(wantColumns), wantColumns)
			}
			for j, want := range wantColumns {
				if strings.TrimSpace(fields[j]) != want {
					t.Fatalf("fixture line %d: column %d is %q, want %q -- "+
						"the fixture was regenerated with a different column "+
						"order; fix the parser rather than the header",
						lineNo, j, fields[j], want)
				}
			}
			continue
		}

		if len(fields) != len(wantColumns) {
			t.Fatalf("fixture line %d (%s): %d columns, want %d",
				lineNo, fields[0], len(fields), len(wantColumns))
		}
		row := ogkmRow{tag: strings.TrimSpace(fields[0]), line: lineNo}
		dst := []*uint32{&row.size, &row.fdOff, &row.hClientOff, &row.hSmcOff, &row.statusOff}
		for j, p := range dst {
			cell := strings.TrimSpace(fields[j+1])
			n, err := strconv.ParseUint(cell, 10, 32)
			if err != nil {
				t.Fatalf("fixture line %d (tag %s): column %q is %q, not a "+
					"measured number -- an unmeasured cell must not be read "+
					"as agreement",
					lineNo, row.tag, wantColumns[j+1], cell)
			}
			*p = uint32(n)
		}
		rows = append(rows, row)
	}

	if !sawHeader {
		t.Fatal("fixture has no column header line")
	}
	if len(rows) == 0 {
		t.Fatal("fixture has no measured rows")
	}
	return rows, meta
}

// TestOGKMFixtureCoverage checks the fixture actually covers the tag set its
// own provenance header claims.  A fixture trimmed to a handful of rows would
// otherwise still make every comparison below pass.
func TestOGKMFixtureCoverage(t *testing.T) {
	rows, meta := loadOGKMFixture(t)

	want, err := strconv.Atoi(meta["tags"])
	if err != nil {
		t.Fatalf("fixture provenance header has no parseable %q field: %v", "tags", err)
	}
	if len(rows) != want {
		t.Errorf("fixture has %d measured rows but its header claims %d tags",
			len(rows), want)
	}
	if want < 100 {
		t.Errorf("fixture claims only %d tags: the whole point is that the "+
			"claim covers every published OGKM tag, not a sample", want)
	}

	seen := map[string]int{}
	for _, r := range rows {
		if prev, dup := seen[r.tag]; dup {
			t.Errorf("tag %s appears twice (lines %d and %d)", r.tag, prev, r.line)
		}
		seen[r.tag] = r.line
	}
	t.Logf("fixture: %d tags, %s .. %s, generated %s",
		len(rows), rows[0].tag, rows[len(rows)-1].tag, meta["generated"])
}

// TestOGKMRegisterGPULayoutIsInvariant is the measurement that licenses
// NVKVM_UVM_REGISTER_GPU_* being plain constants instead of a per-version row
// in struct nvkvm_abi_profile.  If a future regeneration finds a boundary, this
// fails and names every tag at which the tuple changes -- which is the signal
// to move these three numbers into nvkvm_abi_profile.
func TestOGKMRegisterGPULayoutIsInvariant(t *testing.T) {
	rows, _ := loadOGKMFixture(t)

	counts := map[string]int{}
	var boundaries []string
	prev := ""
	for _, r := range rows {
		l := r.layout()
		counts[l]++
		if l != prev {
			boundaries = append(boundaries, fmt.Sprintf("%s: %s", r.tag, l))
			prev = l
		}
	}

	if len(counts) != 1 {
		var layouts []string
		for l, n := range counts {
			layouts = append(layouts, fmt.Sprintf("%s (%d tags)", l, n))
		}
		sort.Strings(layouts)
		t.Errorf("UVM_REGISTER_GPU is NOT version-invariant: %d distinct layouts\n"+
			"  layouts:\n    %s\n  boundaries:\n    %s\n"+
			"  these three numbers must move into struct nvkvm_abi_profile",
			len(counts), strings.Join(layouts, "\n    "),
			strings.Join(boundaries, "\n    "))
		return
	}
	t.Logf("one layout across all %d tags: %s", len(rows), rows[0].layout())
}

// TestUVMRegisterGPUWireConstants checks the constants nvkvm actually puts on
// the wire -- NVKVM_UVM_REGISTER_GPU_{SIZE,FD_OFF,STATUS_OFF} in
// src/common/nvkvm_abi.h -- against the measured driver headers.
//
// A test of this name used to live in abi_parity_test.go and asserted
// sizeof(struct nvkvm_uvm_state_snapshot) == 1176, touching no REGISTER_GPU
// value at all.  That assertion still runs, under an accurate name, as
// TestNvkvmUvmStateSnapshotSize.
func TestUVMRegisterGPUWireConstants(t *testing.T) {
	rows, _ := loadOGKMFixture(t)

	type field struct {
		name string
		got  uint32
		want func(ogkmRow) uint32
	}
	fields := []field{
		{"NVKVM_UVM_REGISTER_GPU_SIZE", RegisterGPUWire.Size, func(r ogkmRow) uint32 { return r.size }},
		{"NVKVM_UVM_REGISTER_GPU_FD_OFF", RegisterGPUWire.FdOff, func(r ogkmRow) uint32 { return r.fdOff }},
		{"NVKVM_UVM_REGISTER_GPU_STATUS_OFF", RegisterGPUWire.StatusOff, func(r ogkmRow) uint32 { return r.statusOff }},
	}
	for _, f := range fields {
		var bad []string
		for _, r := range rows {
			if w := f.want(r); w != f.got {
				bad = append(bad, fmt.Sprintf("%s wants %d", r.tag, w))
			}
		}
		if len(bad) > 0 {
			t.Errorf("%s = %d, but %d/%d measured OGKM tags disagree: %s",
				f.name, f.got, len(bad), len(rows), strings.Join(bad[:minInt(len(bad), 8)], ", "))
		}
	}
}

// TestUVMRegisterGPUStructMatchesOGKM is the conformance half: our own
// struct uvm_register_gpu_params, as laid out by the C compiler, against the
// measured NVIDIA layout at every tag.  This is what catches a field added in
// the wrong place, a wrong-width handle type, or padding we did not expect --
// none of which changes the total size.
func TestUVMRegisterGPUStructMatchesOGKM(t *testing.T) {
	rows, _ := loadOGKMFixture(t)
	ref := rows[0] // TestOGKMRegisterGPULayoutIsInvariant proves all rows agree

	checkSizes(t, []sizeCase{
		{"uvm_register_gpu_params", Sizes.UvmRegGpu, uintptr(ref.size)},
		{"uvm_register_gpu_params.rm_ctrl_fd offset", Sizes.UvmRegGpuFdOff, uintptr(ref.fdOff)},
		{"uvm_register_gpu_params.h_client offset", Sizes.UvmRegGpuHClientOff, uintptr(ref.hClientOff)},
		{"uvm_register_gpu_params.h_smc_part_ref offset", Sizes.UvmRegGpuHSmcPartRefOff, uintptr(ref.hSmcOff)},
		{"uvm_register_gpu_params.rm_status offset", Sizes.UvmRegGpuStatusOff, uintptr(ref.statusOff)},
	})
}

func minInt(a, b int) int {
	if a < b {
		return a
	}
	return b
}
