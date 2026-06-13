#!/usr/bin/env python3
"""
opal_nodes.py — second-layer decoder for Opal/Zouna node payloads extracted by
opal_bigfile.py.

Two things are reliably decodable from any node without per-type layouts:

  1. The common Object_Z header:
        [0x00:0x08] u64  instance id
        [0x16:0x1A] f32  base scalar (1.0 for plain ref types; a real radius/
                         scale for spatial types)
  2. The inter-node reference graph: a node references another when its payload
     contains that node's 8-byte instance id. This recovers, e.g.,
     Material_Z -> Texture_Z / Shader_Z and Node_Z -> children, which makes the
     extracted asset set navigable (and is what an offline server needs to
     resolve dependencies).

Per-type binary sub-formats (texture pixels, mesh vertex/index buffers, …) are
decoded by dedicated functions registered in DECODERS as they are reversed;
see docs/OPAL_BIGFILE_FORMAT.md.

Usage:
    opal_nodes.py <extract_dir>            # summarise headers + reference graph
    opal_nodes.py <extract_dir> --graph G  # write the reference graph to G.csv
"""
import os, sys, csv, struct, argparse, collections

def u64(b, o=0): return struct.unpack_from('<Q', b, o)[0]
def f32(b, o):   return struct.unpack_from('<f', b, o)[0]

def common_header(payload):
    """Return (instance_id, base_scalar) from the Object_Z header, or None."""
    if len(payload) < 0x1a:
        return None
    return u64(payload, 0), f32(payload, 0x16)

def load_manifest(extract_dir):
    """Read manifest.csv -> list of node dicts with file paths."""
    nodes = []
    with open(os.path.join(extract_dir, "manifest.csv")) as fh:
        for r in csv.DictReader(fh):
            r["payload_bytes"] = int(r["payload_bytes"])
            nodes.append(r)
    return nodes

def build_reference_graph(extract_dir, nodes, scan_bytes=512):
    """Edges (src_node_id -> dst_node_id) where src's payload header embeds the
    instance id of dst. Scans the first `scan_bytes` of each payload."""
    # Map every known instance id (8-byte LE) -> its node row.
    id_bytes = {}
    for n in nodes:
        try:
            iid = int(n["node_id"], 16)
        except ValueError:
            continue
        id_bytes[struct.pack('<Q', iid)] = n["node_id"]
    edges = []
    type_edges = collections.Counter()
    for n in nodes:
        path = node_path(extract_dir, n)
        if not path:
            continue
        with open(path, 'rb') as fh:
            head = fh.read(scan_bytes)
        self_id = struct.pack('<Q', int(n["node_id"], 16))
        seen = set()
        for o in range(0, len(head) - 8):
            key = head[o:o+8]
            if key == self_id or key in seen:
                continue
            dst = id_bytes.get(key)
            if dst:
                seen.add(key)
                edges.append((n["node_id"], n["type_name"], dst))
                type_edges[(n["type_name"], _type_of(dst, id_bytes, nodes))] += 1
    return edges, type_edges

def _type_of(node_id, id_bytes, nodes):
    for n in nodes:
        if n["node_id"] == node_id:
            return n["type_name"]
    return "?"

def node_path(extract_dir, n):
    pdir = os.path.join(extract_dir, n["package"])
    if not os.path.isdir(pdir):
        return None
    prefix = f"c{n['chunk']}_{int(n['index']):05d}_"
    for fn in os.listdir(pdir):
        if fn.startswith(prefix):
            return os.path.join(pdir, fn)
    return None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("extract_dir", help="output dir of opal_bigfile.py --batch")
    ap.add_argument("--graph", metavar="OUT", help="write reference graph CSV")
    a = ap.parse_args()

    nodes = load_manifest(a.extract_dir)
    print(f"nodes: {len(nodes)}")
    by_type = collections.Counter(n["type_name"] for n in nodes)
    print("\ntop types:")
    for t, c in by_type.most_common(12):
        print(f"  {t:<22} {c}")

    edges, type_edges = build_reference_graph(a.extract_dir, nodes)
    print(f"\nreference edges: {len(edges)}")
    print("dominant type->type references:")
    for (s, d), c in type_edges.most_common(15):
        print(f"  {s:<20} -> {d:<20} {c}")

    if a.graph:
        with open(a.graph, "w") as fh:
            fh.write("src_id,src_type,dst_id\n")
            for s, st, d in edges:
                fh.write(f"{s},{st},{d}\n")
        print(f"\nwrote {len(edges)} edges -> {a.graph}")

if __name__ == "__main__":
    main()
