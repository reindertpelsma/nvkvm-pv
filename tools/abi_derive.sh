#!/usr/bin/env bash
# abi_derive.sh — MEASURE the per-driver-version ABI profile values by compiling
# sizeof/offsetof probes against NVIDIA open-gpu-kernel-modules at each tag.
#
# WHY THIS EXISTS.  src/common/nvkvm_abi.h carries a version-keyed table of
# struct sizes and offsets.  Those numbers were originally DERIVED by arithmetic
# ("V550 grew the array +9180 bytes, so 535 must be 9264-9180 = 84") rather than
# measured.  Three of the five 535-row values were wrong -- uvm_map_ext_size is
# really 1200, not 84 -- and the error is SILENT: a wrong size does not fail to
# compile, it forwards a truncated struct and the kernel reads past the buffer.
#
# So: never hand-derive a row.  Run this and paste what it prints.
#
# EVIDENCE DISCIPLINE.  Every cell is either MEASURED (a probe compiled and ran)
# or MISSING (printed literally as "MISSING", with the compiler error kept in
# the workdir).  Nothing is ever interpolated from a neighbouring branch, and a
# struct that does not exist at a tag must not silently inherit the previous
# tag's number.  That is why each field is its own translation unit: one absent
# struct then costs one cell, not a whole row.
#
# Usage:
#   tools/abi_derive.sh                     # full branch matrix -> table on stdout
#   tools/abi_derive.sh --tags "535.183.01 580.95.05"
#   tools/abi_derive.sh --format csv        # table | csv | tsv
#   tools/abi_derive.sh --jobs 4            # parallel tags (default: nproc, max 8)
#   tools/abi_derive.sh --work DIR --keep   # keep the clones + probe errors
#   tools/abi_derive.sh --reference-check   # only the 5 rows nvkvm_abi.h cites
#   tools/abi_derive.sh --all-published-supported # every numeric 515..610 tag
#
# Env equivalents: OGKM_TAGS, OGKM_WORK, OGKM_JOBS.
set -uo pipefail

# ---------------------------------------------------------------------------
# The authoritative tag set.
#
# One "latest" tag per OGKM branch, plus an EARLY tag for every branch so that
# intra-branch drift is measured rather than assumed.  A profile keyed to "535"
# is wrong if 535.43 and 535.309 disagree, and nothing but a probe can tell you
# that.  OGKM starts at 515 (NVIDIA's first open release); 470 and earlier have
# no public source and CANNOT be derived by this tool -- they are a documented
# gap, not a zero.
# ---------------------------------------------------------------------------
DEFAULT_TAGS="
515.43.04 515.105.01
520.56.06 520.61.07
525.47.04 525.147.05
530.30.02 530.41.03
535.43.02 535.54.03 535.183.01 535.309.01
545.23.06 545.29.06
550.40.07 550.54.14 550.163.01
555.42.02 555.58.02
560.28.03 560.35.03
565.57.01 565.77
570.86.15 570.172.08 570.211.01
575.51.02 575.64.05
580.65.06 580.95.05 580.178.04
590.44.01 590.48.01
595.44.02 595.91.07
610.43.02 610.57.04
"

# The five rows nvkvm_abi.h says it was built from.  --reference-check re-derives
# exactly these so a maintainer can confirm the method before trusting new rows.
REFERENCE_TAGS="535.183.01 550.54.14 565.57.01 570.172.08 580.95.05"

TAGS="${OGKM_TAGS:-}"
WORK="${OGKM_WORK:-}"
JOBS="${OGKM_JOBS:-}"
FORMAT="table"
KEEP=0
ALL_PUBLISHED_SUPPORTED=0
REPO="https://github.com/NVIDIA/open-gpu-kernel-modules.git"

while [ $# -gt 0 ]; do
	case "$1" in
	--tags) TAGS="$2"; shift 2 ;;
	--work) WORK="$2"; shift 2 ;;
	--jobs) JOBS="$2"; shift 2 ;;
	--format) FORMAT="$2"; shift 2 ;;
	--keep) KEEP=1; shift ;;
	--reference-check) TAGS="$REFERENCE_TAGS"; shift ;;
	--all-published-supported) ALL_PUBLISHED_SUPPORTED=1; shift ;;
	-h|--help) sed -n '2,30p' "$0"; exit 0 ;;
	*) echo "abi_derive.sh: unknown argument '$1'" >&2; exit 2 ;;
	esac
done

if [ "$ALL_PUBLISHED_SUPPORTED" -eq 1 ]; then
	remote_tags=$(git ls-remote --refs --tags "$REPO") || {
		echo "abi_derive.sh: could not list official OGKM tags" >&2
		exit 1
	}
	TAGS=$(printf '%s\n' "$remote_tags" |
		awk '{ sub("refs/tags/", "", $2); print $2 }' |
		grep -E '^[0-9]+(\.[0-9]+){1,2}$' |
		awk -F. '$1 >= 515 && $1 <= 610' |
		sort -V)
	[ -n "$TAGS" ] || {
		echo "abi_derive.sh: official tag query returned no numeric 515..610 tags" >&2
		exit 1
	}
fi

[ -n "$TAGS" ] || TAGS="$DEFAULT_TAGS"
[ -n "$WORK" ] || WORK="$(mktemp -d "${TMPDIR:-/tmp}/ogkm-derive.XXXXXX")"
if [ -z "$JOBS" ]; then
	JOBS="$(nproc 2>/dev/null || echo 2)"
	[ "$JOBS" -gt 8 ] && JOBS=8
fi
mkdir -p "$WORK"

# ---------------------------------------------------------------------------
# Field table: <name>|<headers,comma-sep>|<C expression>[|<headers>|<expr> ...]
#
# Most field names match members of `struct nvkvm_abi_profile` in
# src/common/nvkvm_abi.h one-for-one, so a row here transcribes into that table
# without a rename step (renames are where transcription bugs live).  Fields
# prefixed `uvm_register_gpu_` additionally prove the supposedly universal
# REGISTER_GPU layout at every tag.  Keeping them in this same sweep prevents a
# fixed offset in guest/QEMU/stub code from quietly crossing a driver boundary.
#
# A field may list SEVERAL header/expression alternatives, tried left to right;
# the first that compiles wins and the table marks the row with the alternative
# index.  This is for RENAMES ONLY -- the same struct under a different spelling
# at an older tag -- never for a substitute struct.  Two real cases:
#
#   chan_alloc_size: pre-525 there is no alloc/alloc_channel.h and the type is
#     spelled NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS in nvos.h.  Measured 515
#     and 520 through that spelling; it is the identical GPFIFO alloc-param
#     struct, so this is a measurement, not an inference.
#
# A field whose class genuinely does not exist at a tag stays MISSING: e.g. no
# tag before 525 has NV00DE / RM_USER_SHARED_DATA anywhere in the tree (grep
# confirms zero hits), so nv00de_alloc_size is *absent*, not merely unprobed.
# ---------------------------------------------------------------------------
FIELDS_FILE="$WORK/fields.txt"
cat > "$FIELDS_FILE" <<'F'
uvm_map_ext_size|uvm_types.h,uvm_ioctl.h|sizeof(UVM_MAP_EXTERNAL_ALLOCATION_PARAMS)
uvm_map_ext_fd_off|uvm_types.h,uvm_ioctl.h|offsetof(UVM_MAP_EXTERNAL_ALLOCATION_PARAMS, rmCtrlFd)
uvm_sem_pool_size|uvm_types.h,uvm_ioctl.h|sizeof(UVM_ALLOC_SEMAPHORE_POOL_PARAMS)
uvm_register_gpu_size|uvm_types.h,uvm_ioctl.h|sizeof(UVM_REGISTER_GPU_PARAMS)
uvm_register_gpu_fd_off|uvm_types.h,uvm_ioctl.h|offsetof(UVM_REGISTER_GPU_PARAMS, rmCtrlFd)
uvm_register_gpu_status_off|uvm_types.h,uvm_ioctl.h|offsetof(UVM_REGISTER_GPU_PARAMS, rmStatus)
uvm_register_gpu_hclient_off|uvm_types.h,uvm_ioctl.h|offsetof(UVM_REGISTER_GPU_PARAMS, hClient)
uvm_register_gpu_hsmcpart_off|uvm_types.h,uvm_ioctl.h|offsetof(UVM_REGISTER_GPU_PARAMS, hSmcPartRef)
chan_alloc_size|nvtypes.h,nvos.h,alloc/alloc_channel.h|sizeof(NV_CHANNEL_ALLOC_PARAMS)|nvtypes.h,nvos.h|sizeof(NV_CHANNELGPFIFO_ALLOCATION_PARAMETERS)
vaspace_alloc_size|nvtypes.h,nvos.h|sizeof(NV_VASPACE_ALLOCATION_PARAMETERS)
mem_alloc_size|nvtypes.h,nvos.h|sizeof(NV_MEMORY_ALLOCATION_PARAMS)
nv00de_alloc_size|nvtypes.h,nvos.h,class/cl00de.h|sizeof(NV00DE_ALLOC_PARAMETERS)
nvos46_size|nvtypes.h,nvos.h|sizeof(NVOS46_PARAMETERS)
nvos46_status_off|nvtypes.h,nvos.h|offsetof(NVOS46_PARAMETERS, status)
nvos32_osdesc_desc_off|nvtypes.h,nvos.h|offsetof(NVOS32_PARAMETERS, data.AllocOsDesc.descriptor)
nvos32_osdesc_limit_off|nvtypes.h,nvos.h|offsetof(NVOS32_PARAMETERS, data.AllocOsDesc.limit)
F
FIELD_NAMES=$(cut -d'|' -f1 "$FIELDS_FILE" | tr '\n' ' ')

# ---------------------------------------------------------------------------
# measure_tag TAG -> writes "$WORK/out/TAG" as "field=value" lines.
#
# Clone is blobless (--filter=blob:none) + sparse: only the four header trees
# the probes include are materialised, which is ~30 MB per tag instead of ~1 GB
# and makes a 36-tag sweep cheap enough to rerun on every driver release.
# ---------------------------------------------------------------------------
measure_tag() {
	local tag="$1"
	local src="$WORK/src/$tag"
	local log="$WORK/log/$tag"
	local out="$WORK/out/$tag"
	mkdir -p "$(dirname "$out")" "$log"

	if [ ! -d "$src/.git" ]; then
		rm -rf "$src"
		if ! git clone -q --depth 1 --filter=blob:none --sparse \
			--branch "$tag" "$REPO" "$src" >"$log/clone.err" 2>&1; then
			echo "__clone_failed__=1" > "$out"
			return 0
		fi
		( cd "$src" && git sparse-checkout set \
			kernel-open/nvidia-uvm kernel-open/common/inc \
			src/common/sdk/nvidia/inc src/common/inc ) \
			>>"$log/clone.err" 2>&1 || { echo "__clone_failed__=1" > "$out"; return 0; }
	fi

	local inc=(
		-I"$src/kernel-open/nvidia-uvm"
		-I"$src/kernel-open/common/inc"
		-I"$src/src/common/sdk/nvidia/inc"
		-I"$src/src/common/inc"
	)

	: > "$out"
	local spec name h alt nalt hdrs expr got
	while IFS= read -r spec; do
		[ -n "$spec" ] || continue
		name=${spec%%|*}
		# Remaining fields are (headers, expr) pairs: alternative spellings.
		IFS='|' read -r -a parts <<< "$spec"
		nalt=$(( (${#parts[@]} - 1) / 2 ))
		got=""
		for alt in $(seq 0 $((nalt - 1))); do
			hdrs=${parts[$((1 + alt * 2))]}
			expr=${parts[$((2 + alt * 2))]}
			{
				echo '#include <stdio.h>'
				echo '#include <stddef.h>'
				for h in ${hdrs//,/ }; do echo "#include \"$h\""; done
				echo "int main(void){ printf(\"%zu\\n\", (size_t)($expr)); return 0; }"
			} > "$log/$name.alt$alt.c"

			if gcc -w -o "$log/$name.alt$alt.bin" "$log/$name.alt$alt.c" "${inc[@]}" \
				>"$log/$name.alt$alt.err" 2>&1 \
				&& "$log/$name.alt$alt.bin" > "$log/$name.alt$alt.val" 2>&1; then
				got=$(cat "$log/$name.alt$alt.val")
				# Suffix '*' marks a value measured through a fallback spelling.
				[ "$alt" -eq 0 ] || got="$got*"
				break
			fi
		done
		# MISSING, never inferred.  Errors stay in $log/$name.alt*.err.
		echo "$name=${got:-MISSING}" >> "$out"
	done < "$FIELDS_FILE"

	[ "$KEEP" -eq 1 ] || rm -rf "$src"
	return 0
}

# ---------------------------------------------------------------------------
# Run the sweep.
# ---------------------------------------------------------------------------
mkdir -p "$WORK/src" "$WORK/log" "$WORK/out"
TAG_LIST=$(echo $TAGS)
echo "abi_derive: $(echo "$TAG_LIST" | wc -w) tags, ${JOBS} parallel, work=$WORK" >&2

running=0
for t in $TAG_LIST; do
	measure_tag "$t" &
	running=$((running + 1))
	if [ "$running" -ge "$JOBS" ]; then wait -n 2>/dev/null || wait; running=$((running - 1)); fi
done
wait

# ---------------------------------------------------------------------------
# Emit the matrix.
# ---------------------------------------------------------------------------
emit_row() {  # tag, then values on stdout separated by $SEP
	local tag="$1" out="$WORK/out/$1" f v
	local row="$tag"
	if [ ! -s "$out" ] || grep -q '__clone_failed__' "$out" 2>/dev/null; then
		for f in $FIELD_NAMES; do row="$row${SEP}CLONE_FAIL"; done
	else
		for f in $FIELD_NAMES; do
			v=$(grep "^$f=" "$out" | head -1 | cut -d= -f2)
			[ -n "$v" ] || v="MISSING"
			row="$row${SEP}$v"
		done
	fi
	echo "$row"
}

case "$FORMAT" in
csv) SEP=","
     echo "tag,$(echo $FIELD_NAMES | tr ' ' ',')"
     for t in $TAG_LIST; do emit_row "$t"; done ;;
tsv) SEP=$'\t'
     printf 'tag\t%s\n' "$(echo $FIELD_NAMES | tr ' ' '\t')"
     for t in $TAG_LIST; do emit_row "$t"; done ;;
*)   SEP=$'\t'
     { printf 'driver\t%s\n' "$(echo $FIELD_NAMES | tr ' ' '\t')"
       for t in $TAG_LIST; do emit_row "$t"; done ; } | column -t -s $'\t' ;;
esac

# ---------------------------------------------------------------------------
# Distinct-layout summary: the actual deliverable.  Two tags with an identical
# measured tuple need one profile between them; a change in the tuple IS a profile
# boundary, and the boundary is only real where a probe measured it.
# ---------------------------------------------------------------------------
if [ "$FORMAT" = "table" ]; then
	echo
	echo "── distinct layouts (identical measured tuple ⇒ one profile) ──"
	SEP=" "
	prev=""
	for t in $TAG_LIST; do
		sig=$(emit_row "$t" | cut -d' ' -f2-)
		if [ "$sig" != "$prev" ]; then
			echo "  boundary at $t: $sig"
			prev="$sig"
		fi
	done
	echo
	echo "Clones/probe logs: $WORK   (--keep to retain sources)"
fi

[ "$KEEP" -eq 1 ] || [ -n "${OGKM_WORK:-}" ] || rm -rf "$WORK/src"
