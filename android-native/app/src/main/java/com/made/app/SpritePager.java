package com.made.app;

import android.animation.Animator;
import android.animation.AnimatorListenerAdapter;
import android.animation.ValueAnimator;
import android.content.Context;
import android.widget.FrameLayout;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * 三屏精灵分页（跟手）：
 * - 中心 = 当前助手，左右各预载一页（素材提前拉取，一次加载 3 屏）；
 * - 手指拖动直接改 translationX（跟手），松手按位移吸附换页或回弹；
 * - 页面 = SpriteView，按 providerId 缓存，最多保留中心 ±1 共 3 个；
 * - 边界无邻页时橡皮筋减幅。
 */
public final class SpritePager extends FrameLayout {
    private final Bridge bridge;
    private final Map<String, SpriteView> pages = new HashMap<>();
    private final List<String> pageOrder = new ArrayList<>();
    private List<Model.Provider> providers = new ArrayList<>();
    private int center = 0;
    private float drag = 0f;
    private boolean snapping = false;
    private boolean dragging = false;
    private SpriteView.ActionsListener actionsListener;

    public SpritePager(Context c, Bridge b) {
        super(c);
        bridge = b;
    }

    public void setActionsListener(SpriteView.ActionsListener l) { actionsListener = l; }

    public SpriteView current() { return pageAt(center); }

    public List<String> currentCommandNames() {
        SpriteView v = current();
        return v == null ? new ArrayList<>() : v.availableCommandNames();
    }

    private SpriteView pageAt(int idx) {
        return valid(idx) ? pages.get(providers.get(idx).id) : null;
    }

    private boolean valid(int idx) { return idx >= 0 && idx < providers.size(); }

    /** 同步提供者表；拖动/吸附进行中只更新快照，绝不动视觉（防轮询打断跟手）。 */
    public void sync(List<Model.Provider> provs, int selIdx) {
        providers = new ArrayList<>(provs);
        if (dragging || snapping) return;
        if (!provs.isEmpty()) {
            center = wrapIdx(selIdx);
        }
        drag = 0f;
        refreshPages();
        positionChildren();
    }

    /** 拖动中：px = 手指相对按下点的位移（负 = 往下一页）。超一屏软钳制。 */
    public void setDrag(float px) {
        if (snapping) return;
        dragging = true;
        float lim = 1.15f * Math.max(1, getWidth());
        drag = Math.max(-lim, Math.min(lim, px));
        positionChildren();
    }

    /** 手指离开：结束拖动态（其后 snapTo 才允许接管视觉）。 */
    public void endDrag() {
        dragging = false;
    }

    /** 松手：吸附到 target（环形；target == center 即回弹）。end 让 dest 页
     * 落在 0：wrapped(dest)*w + end = 0 → end = -wrappedOffset(dest)*w。
     * 符号或基准写反都会把别的页扫进屏再跳变（v10 教训）。 */
    public void snapTo(int targetRaw) {
        int target = providers.isEmpty() ? center : wrapIdx(targetRaw);
        final int dest = target;
        float start = drag;
        float end = -wrappedOffset(dest) * Math.max(1, getWidth());
        dragging = false;
        snapping = true;
        ValueAnimator a = ValueAnimator.ofFloat(start, end);
        a.setDuration(200);
        a.setInterpolator(new android.view.animation.DecelerateInterpolator(1.2f));
        a.addUpdateListener(anim -> {
            drag = (float) anim.getAnimatedValue();
            positionChildren();
        });
        a.addListener(new AnimatorListenerAdapter() {
            @Override public void onAnimationEnd(Animator animation) {
                center = dest;
                drag = 0f;
                snapping = false;
                refreshPages();
                positionChildren();
            }
        });
        a.start();
    }

    /** 索引环形归一（负数/越界都回到 0..n-1）。 */
    private int wrapIdx(int idx) {
        int n = providers.size();
        if (n == 0) return 0;
        return ((idx % n) + n) % n;
    }

    /** 页 i 相对 center 的最短环绕距离（页数 n 时范围 [-n/2, n/2]）。
     *  环形分页核心：center=最后一页时，第一页的 offset 是 +1（右邻）。 */
    private int wrappedOffset(int i) {
        int n = Math.max(1, providers.size());
        int d = i - center;
        while (d > n / 2) d -= n;
        while (d < -(n / 2)) d += n;
        return d;
    }

    /** 幂等刷新：中心环形 ±1 页创建/版本同步/文案，超过 3 页回收。 */
    private void refreshPages() {
        ensurePage(wrapIdx(center));
        ensurePage(wrapIdx(center - 1));
        ensurePage(wrapIdx(center + 1));
        evictFar();
    }

    /** 拖动开始时调用：保证环形邻页就绪（不动 drag/center，不影响跟手）。 */
    public void ensureNeighbors() {
        ensurePage(wrapIdx(center));
        ensurePage(wrapIdx(center - 1));
        ensurePage(wrapIdx(center + 1));
    }

    private void ensurePage(int idx) {
        if (!valid(idx)) return;
        Model.Provider p = providers.get(idx);
        SpriteView v = pages.get(p.id);
        if (v != null) {
            v.setProvider(p.id, p.spriteVersion); // 幂等；版本变化触发重下
            v.setLabel(p.label);
            return;
        }
        v = new SpriteView(getContext(), bridge);
        // 先定位再挂上：新页若有一帧落在 tx=0 会盖住当前页（闪屏根源）
        addView(v, new LayoutParams(-1, -1));
        pages.put(p.id, v);
        pageOrder.add(p.id);
        if (actionsListener != null) v.setActionsListener(actionsListener);
        v.setProvider(p.id, p.spriteVersion);
        v.setLabel(p.label);
        positionChildren();
    }

    private void evictFar() {
        if (pages.size() <= 3) return;
        List<String> keep = new ArrayList<>();
        for (int i = center - 1; i <= center + 1; i++) {
            keep.add(providers.get(wrapIdx(i)).id);
        }
        List<String> doomed = new ArrayList<>();
        for (String id : pageOrder) if (!keep.contains(id)) doomed.add(id);
        for (String id : doomed) {
            SpriteView v = pages.remove(id);
            pageOrder.remove(id);
            if (v != null) removeView(v);
        }
    }

    private void positionChildren() {
        int w = Math.max(1, getWidth());
        for (int i = 0; i < providers.size(); i++) {
            SpriteView v = pages.get(providers.get(i).id);
            if (v == null) continue;
            // 环形：最短环绕距离定位（首尾互为邻居），边界不再需要橡皮筋
            v.setTranslationX(wrappedOffset(i) * w + drag);
            // 始终 VISIBLE：可见性翻转会在跟手拖动时造成首帧闪动
        }
    }
}
