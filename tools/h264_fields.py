# SPDX-FileCopyrightText: 2026 ElasticSea
# SPDX-License-Identifier: Apache-2.0
"""Dump every SPS, PPS and slice-header field of an Annex-B H.264 stream (ITU-T H.264 7.3).

Used to compare encoders field by field (e.g. the NVIDIA MFT vs x264 clips).

Usage: py h264_fields.py <a.h264> [b.h264]
  one file:  per-NAL field dump (SPS/PPS once each distinct, first slices of an IDR and a P frame)
  two files: only the fields that differ, per NAL kind
"""
import sys

from decode_video import Bits, nal_units, rbsp

HIGH_PROFILES = (100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134, 135)


class Reader(Bits):
    def more_rbsp_data(self):
        """True while bits other than the rbsp_stop_one_bit and its trailing zeros remain."""
        last = len(self.d) - 1
        while last >= 0 and self.d[last] == 0:
            last -= 1
        if last < 0:
            return False
        stop = last * 8 + 7 - (self.d[last] & -self.d[last]).bit_length() + 1
        return self.p < stop


def hrd(b, f, pre):
    f[pre + "cpb_cnt_minus1"] = n = b.ue()
    f[pre + "bit_rate_scale"] = b.u(4)
    f[pre + "cpb_size_scale"] = b.u(4)
    for i in range(n + 1):
        f[f"{pre}bit_rate_value_minus1[{i}]"] = b.ue()
        f[f"{pre}cpb_size_value_minus1[{i}]"] = b.ue()
        f[f"{pre}cbr_flag[{i}]"] = b.u(1)
    for k in ("initial_cpb_removal_delay_length_minus1", "cpb_removal_delay_length_minus1",
              "dpb_output_delay_length_minus1", "time_offset_length"):
        f[pre + k] = b.u(5)


def vui(b, f):
    p = "vui."
    if b.u(1):
        f[p + "aspect_ratio_idc"] = idc = b.u(8)
        if idc == 255:
            f[p + "sar"] = (b.u(16), b.u(16))
    if b.u(1):
        f[p + "overscan_appropriate_flag"] = b.u(1)
    f[p + "video_signal_type_present_flag"] = vs = b.u(1)
    if vs:
        f[p + "video_format"] = b.u(3)
        f[p + "video_full_range_flag"] = b.u(1)
        if b.u(1):
            f[p + "colour"] = (b.u(8), b.u(8), b.u(8))
    if b.u(1):
        f[p + "chroma_sample_loc"] = (b.ue(), b.ue())
    f[p + "timing_info_present_flag"] = t = b.u(1)
    if t:
        f[p + "num_units_in_tick"] = b.u(32)
        f[p + "time_scale"] = b.u(32)
        f[p + "fixed_frame_rate_flag"] = b.u(1)
    f[p + "nal_hrd_parameters_present_flag"] = nal_hrd = b.u(1)
    if nal_hrd:
        hrd(b, f, p + "nal_hrd.")
    f[p + "vcl_hrd_parameters_present_flag"] = vcl_hrd = b.u(1)
    if vcl_hrd:
        hrd(b, f, p + "vcl_hrd.")
    if nal_hrd or vcl_hrd:
        f[p + "low_delay_hrd_flag"] = b.u(1)
    f[p + "pic_struct_present_flag"] = b.u(1)
    f[p + "bitstream_restriction_flag"] = r = b.u(1)
    if r:
        for k in ("motion_vectors_over_pic_boundaries_flag",):
            f[p + k] = b.u(1)
        for k in ("max_bytes_per_pic_denom", "max_bits_per_mb_denom", "log2_max_mv_length_horizontal",
                  "log2_max_mv_length_vertical", "max_num_reorder_frames", "max_dec_frame_buffering"):
            f[p + k] = b.ue()


def sps_fields(nal):
    b = Reader(rbsp(nal))
    f = {"nal_ref_idc": nal[0] >> 5, "profile_idc": b.u(8), "constraint_flags": hex(b.u(8)),
         "level_idc": b.u(8), "seq_parameter_set_id": b.ue()}
    if f["profile_idc"] in HIGH_PROFILES:
        f["chroma_format_idc"] = b.ue()
        if f["chroma_format_idc"] == 3:
            f["separate_colour_plane_flag"] = b.u(1)
        f["bit_depth_luma_minus8"], f["bit_depth_chroma_minus8"] = b.ue(), b.ue()
        f["qpprime_y_zero_transform_bypass_flag"] = b.u(1)
        if b.u(1):
            raise ValueError("scaling matrices not handled")
    f["log2_max_frame_num_minus4"] = b.ue()
    f["pic_order_cnt_type"] = poc = b.ue()
    if poc == 0:
        f["log2_max_pic_order_cnt_lsb_minus4"] = b.ue()
    elif poc == 1:
        f["delta_pic_order_always_zero_flag"] = b.u(1)
        f["offset_for_non_ref_pic"], f["offset_for_top_to_bottom_field"] = b.se(), b.se()
        f["offset_for_ref_frame"] = [b.se() for _ in range(b.ue())]
    f["max_num_ref_frames"] = b.ue()
    f["gaps_in_frame_num_value_allowed_flag"] = b.u(1)
    f["pic_width_in_mbs_minus1"], f["pic_height_in_map_units_minus1"] = b.ue(), b.ue()
    f["frame_mbs_only_flag"] = fmo = b.u(1)
    if not fmo:
        f["mb_adaptive_frame_field_flag"] = b.u(1)
    f["direct_8x8_inference_flag"] = b.u(1)
    f["frame_cropping_flag"] = crop = b.u(1)
    if crop:
        f["frame_crop"] = (b.ue(), b.ue(), b.ue(), b.ue())
    f["vui_parameters_present_flag"] = v = b.u(1)
    if v:
        vui(b, f)
    return f


def pps_fields(nal):
    b = Reader(rbsp(nal))
    f = {"nal_ref_idc": nal[0] >> 5, "pic_parameter_set_id": b.ue(), "seq_parameter_set_id": b.ue(),
         "entropy_coding_mode_flag": b.u(1), "bottom_field_pic_order_in_frame_present_flag": b.u(1),
         "num_slice_groups_minus1": b.ue()}
    if f["num_slice_groups_minus1"]:
        raise ValueError("slice groups not handled")
    f["num_ref_idx_l0_default_active_minus1"], f["num_ref_idx_l1_default_active_minus1"] = b.ue(), b.ue()
    f["weighted_pred_flag"], f["weighted_bipred_idc"] = b.u(1), b.u(2)
    f["pic_init_qp_minus26"], f["pic_init_qs_minus26"], f["chroma_qp_index_offset"] = b.se(), b.se(), b.se()
    f["deblocking_filter_control_present_flag"] = b.u(1)
    f["constrained_intra_pred_flag"] = b.u(1)
    f["redundant_pic_cnt_present_flag"] = b.u(1)
    if b.more_rbsp_data():
        f["transform_8x8_mode_flag"] = b.u(1)
    return f


def slice_fields(nal, sps, pps):
    """Slice header up to (not including) slice_data, for frame (non-field) pictures."""
    b = Reader(rbsp(nal[:256]))
    t, idc = nal[0] & 0x1F, nal[0] >> 5
    f = {"nal_ref_idc": idc, "nal_unit_type": t, "first_mb_in_slice": b.ue(), "slice_type": b.ue(),
         "pic_parameter_set_id": b.ue()}
    st = f["slice_type"] % 5
    f["frame_num"] = b.u(sps["log2_max_frame_num_minus4"] + 4)
    if not sps["frame_mbs_only_flag"]:
        raise ValueError("field coding not handled")
    if t == 5:
        f["idr_pic_id"] = b.ue()
    if sps["pic_order_cnt_type"] == 0:
        f["pic_order_cnt_lsb"] = b.u(sps["log2_max_pic_order_cnt_lsb_minus4"] + 4)
        if pps["bottom_field_pic_order_in_frame_present_flag"]:
            f["delta_pic_order_cnt_bottom"] = b.se()
    elif sps["pic_order_cnt_type"] == 1 and not sps["delta_pic_order_always_zero_flag"]:
        f["delta_pic_order_cnt[0]"] = b.se()
    if pps["redundant_pic_cnt_present_flag"]:
        f["redundant_pic_cnt"] = b.ue()
    if st == 1:
        f["direct_spatial_mv_pred_flag"] = b.u(1)
    if st in (0, 1, 3):
        f["num_ref_idx_active_override_flag"] = o = b.u(1)
        if o:
            f["num_ref_idx_l0_active_minus1"] = b.ue()
            if st == 1:
                f["num_ref_idx_l1_active_minus1"] = b.ue()
    if st not in (2, 4):
        f["ref_pic_list_modification_flag_l0"] = m = b.u(1)
        if m:
            mods = []
            while (op := b.ue()) != 3:
                mods.append((op, b.ue()))
            f["ref_pic_list_modification_l0"] = mods
    if pps["weighted_pred_flag"] and st in (0, 3):
        raise ValueError("pred_weight_table not handled")
    if idc:
        if t == 5:
            f["no_output_of_prior_pics_flag"], f["long_term_reference_flag"] = b.u(1), b.u(1)
        else:
            f["adaptive_ref_pic_marking_mode_flag"] = a = b.u(1)
            if a:
                ops = []
                while (op := b.ue()) != 0:
                    ops.append((op, b.ue() if op in (1, 2, 3, 6, 4) else None))
                    if op == 3:
                        ops[-1] = (op, ops[-1][1], b.ue())
                f["mmco"] = ops
    if pps["entropy_coding_mode_flag"] and st not in (2, 4):
        f["cabac_init_idc"] = b.ue()
    f["slice_qp_delta"] = b.se()
    if pps["deblocking_filter_control_present_flag"]:
        f["disable_deblocking_filter_idc"] = d = b.ue()
        if d != 1:
            f["slice_alpha_c0_offset_div2"], f["slice_beta_offset_div2"] = b.se(), b.se()
    return f


def analyse(path):
    """-> {"SPS": [fields...], "PPS": [...], "IDR": [first slice fields per IDR], "P": [...], "other": {...}}"""
    with open(path, "rb") as fh:
        es = fh.read()
    out = {"SPS": [], "PPS": [], "IDR": [], "P": [], "nal_types": {}}
    sps = pps = None
    for _, nal in nal_units(es):
        if not nal:
            continue
        t = nal[0] & 0x1F
        out["nal_types"][t] = out["nal_types"].get(t, 0) + 1
        if t == 7:
            sps = sps_fields(nal)
            if sps not in out["SPS"]:
                out["SPS"].append(sps)
        elif t == 8:
            pps = pps_fields(nal)
            if pps not in out["PPS"]:
                out["PPS"].append(pps)
        elif t in (1, 5):
            out["IDR" if t == 5 else "P"].append(slice_fields(nal, sps, pps))
    return out


VOLATILE = ("first_mb_in_slice", "frame_num", "idr_pic_id", "slice_qp_delta", "pic_order_cnt_lsb")


def summary(slices):
    """Per field: the set of values seen over all slices (volatile fields as a range)."""
    vals = {}
    for s in slices:
        for k, v in s.items():
            vals.setdefault(k, set()).add(v if not isinstance(v, list) else tuple(v))
    return {k: (f"{min(v)}..{max(v)}" if k in VOLATILE and len(v) > 2 else sorted(v, key=str))
            for k, v in vals.items()}


def main():
    a = analyse(sys.argv[1])
    if len(sys.argv) == 2:
        print("NAL types:", a["nal_types"])
        for kind in ("SPS", "PPS"):
            for i, f in enumerate(a[kind]):
                print(f"{kind} #{i}:")
                for k, v in f.items():
                    print(f"  {k} = {v}")
        for kind in ("IDR", "P"):
            print(f"{kind} slices ({len(a[kind])}):")
            for k, v in summary(a[kind]).items():
                print(f"  {k} = {v}")
        return
    b = analyse(sys.argv[2])
    print("NAL types:", a["nal_types"], "|", b["nal_types"])
    for kind in ("SPS", "PPS"):
        fa, fb = (a[kind] or [{}])[0], (b[kind] or [{}])[0]
        if len(a[kind]) > 1 or len(b[kind]) > 1:
            print(f"{kind}: {len(a[kind])} vs {len(b[kind])} distinct")
        for k in dict.fromkeys([*fa, *fb]):
            if fa.get(k) != fb.get(k):
                print(f"{kind} {k}: {fa.get(k, '-')} | {fb.get(k, '-')}")
    for kind in ("IDR", "P"):
        sa, sb = summary(a[kind]), summary(b[kind])
        for k in dict.fromkeys([*sa, *sb]):
            if sa.get(k) != sb.get(k):
                print(f"{kind} {k}: {sa.get(k, '-')} | {sb.get(k, '-')}")


if __name__ == "__main__":
    main()
