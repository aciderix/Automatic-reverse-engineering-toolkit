//! Shared dominator-tree utilities used by control-flow structuring and (in
//! progress) SSA construction. Generic over an integer-indexed CFG so the same
//! code serves the forward dominators, the post-dominators (dominators of the
//! reversed graph), and dominance-frontier computation for φ-node placement.

/// Sentinel for "no immediate dominator" (entry / unreachable nodes).
pub const UNDEF: usize = usize::MAX;

/// Reverse-postorder of the graph reachable from `entry` over `succ`, plus a
/// position lookup (`num[node]` = index in the returned order).
pub fn reverse_postorder(n: usize, entry: usize, succ: &[Vec<usize>]) -> (Vec<usize>, Vec<usize>) {
    let mut visited = vec![false; n];
    let mut post = Vec::new();
    let mut stack = vec![(entry, 0usize)];
    visited[entry] = true;
    while let Some(&(node, ci)) = stack.last() {
        if ci < succ[node].len() {
            stack.last_mut().unwrap().1 += 1;
            let nx = succ[node][ci];
            if nx < n && !visited[nx] {
                visited[nx] = true;
                stack.push((nx, 0));
            }
        } else {
            post.push(node);
            stack.pop();
        }
    }
    post.reverse();
    let mut num = vec![UNDEF; n];
    for (i, &node) in post.iter().enumerate() {
        num[node] = i;
    }
    (post, num)
}

/// Immediate dominators via Cooper–Harvey–Kennedy. `idom[entry] == entry`;
/// unreachable nodes keep `UNDEF`.
pub fn dominators(n: usize, entry: usize, succ: &[Vec<usize>], preds: &[Vec<usize>]) -> Vec<usize> {
    let (rpo, num) = reverse_postorder(n, entry, succ);
    let mut idom = vec![UNDEF; n];
    idom[entry] = entry;

    let intersect = |mut a: usize, mut b: usize, idom: &Vec<usize>| -> usize {
        while a != b {
            while num[a] > num[b] {
                a = idom[a];
            }
            while num[b] > num[a] {
                b = idom[b];
            }
        }
        a
    };

    let mut changed = true;
    while changed {
        changed = false;
        for &node in &rpo {
            if node == entry {
                continue;
            }
            let mut new_idom = UNDEF;
            for &p in &preds[node] {
                if idom[p] != UNDEF {
                    new_idom = if new_idom == UNDEF {
                        p
                    } else {
                        intersect(p, new_idom, &idom)
                    };
                }
            }
            if new_idom != UNDEF && idom[node] != new_idom {
                idom[node] = new_idom;
                changed = true;
            }
        }
    }
    idom
}

/// Does `a` dominate `b` (walking up the idom chain from `b`)?
pub fn dominates(a: usize, b: usize, idom: &[usize]) -> bool {
    let mut x = b;
    loop {
        if x == a {
            return true;
        }
        if idom[x] == UNDEF {
            return false;
        }
        let nx = idom[x];
        if nx == x {
            return a == x;
        }
        x = nx;
    }
}

/// Dominance frontiers (Cytron et al.) — the φ-placement set for each node.
/// Used by SSA construction (in progress); kept here as part of the shared CFG
/// toolkit the roadmap calls for.
#[allow(dead_code)]
pub fn dominance_frontiers(n: usize, preds: &[Vec<usize>], idom: &[usize]) -> Vec<Vec<usize>> {
    let mut df: Vec<Vec<usize>> = vec![Vec::new(); n];
    for b in 0..n {
        if preds[b].len() < 2 || idom[b] == UNDEF {
            continue;
        }
        for &p in &preds[b] {
            let mut runner = p;
            while runner != UNDEF && runner != idom[b] {
                if !df[runner].contains(&b) {
                    df[runner].push(b);
                }
                if idom[runner] == runner {
                    break; // reached the entry's self-loop
                }
                runner = idom[runner];
            }
        }
    }
    df
}
