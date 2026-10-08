#include "overlay_selection.h"

#include <algorithm>

namespace screen_capture {
namespace overlay {

bool RectsIntersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    if (ax >= bx + bw || bx >= ax + aw) return false;
    if (ay >= by + bh || by >= ay + ah) return false;
    return true;
}

bool RectInside(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh) {
    return ax >= bx && ay >= by && ax + aw <= bx + bw && ay + ah <= by + bh;
}

std::vector<OverlayPaint> SelectOverlays(const std::vector<OverlayCandidate>& all,
                                         const CaptureFrame& frame) {
    // z хранится рядом с работой, а не ищется потом по hwnd: порядок отрисовки — часть правила,
    // и он не должен зависеть от того, отсортирован ли вход.
    struct Ordered {
        int z;
        OverlayPaint job;
    };
    std::vector<Ordered> ordered;
    ordered.reserve(all.size());

    for (const OverlayCandidate& c : all) {
        // Свой процесс. Это главный инвариант решения: именно он отличает композит от съёмки
        // экрана, и он не должен зависеть от того, что перечислитель уже отфильтровал вход.
        // Ранняя фильтрация в сборщике остаётся оптимизацией, гарантию несёт правило.
        if (c.pid != frame.targetPid) continue;

        // Видимое.
        if (!c.visible) continue;

        // Выше якоря по Z. Окно ниже якоря им же и закрыто, рисовать его поверх базы значит
        // показать то, чего на экране нет. Этим же условием отсекается и сам якорь: у него
        // z == anchorZ, а строгое «меньше» его не пропускает.
        if (c.z >= frame.anchorZ) continue;

        // Не вырожденное: 1С держит вокруг главного окна выводок служебной мелочи.
        if (c.width < kMinOverlaySide || c.height < kMinOverlaySide) continue;

        const int dstX = c.left - frame.originX;
        const int dstY = c.top - frame.originY;
        if (!RectsIntersect(dstX, dstY, c.width, c.height, 0, 0, frame.width, frame.height)) continue;

        OverlayPaint job;
        job.hwnd = c.hwnd;
        job.srcLeft = c.left;
        job.srcTop = c.top;
        job.w = c.width;
        job.h = c.height;
        job.dstX = dstX;
        job.dstY = dstY;
        job.clipped = !RectInside(dstX, dstY, c.width, c.height, 0, 0, frame.width, frame.height);

        // Чем снимать, решает правило, а не отрисовщик. SendMessage в окно своего потока — прямой
        // вызов оконной процедуры: заблокировать не может и завершается вместе с отрисовкой.
        // В окно чужого потока сообщение не посылается вовсе: без таймаута это неограниченное
        // ожидание, а с таймаутом отправитель освобождается по бюджету, тогда как чужая WndProc
        // продолжает рисовать в уже удалённый нами битмап.
        job.backend = (c.tid == frame.selfTid) ? OverlayBackend::PrintWindow : OverlayBackend::Screen;

        Ordered item;
        item.z = c.z;
        item.job = job;
        ordered.push_back(item);
    }

    // Снизу вверх: меню поверх списка должно лечь поверх списка. Перечисление отдаёт Z-порядок
    // сверху вниз, поэтому красим в обратном.
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const Ordered& a, const Ordered& b) { return a.z > b.z; });

    std::vector<OverlayPaint> jobs;
    jobs.reserve(ordered.size());
    for (const Ordered& item : ordered) jobs.push_back(item.job);
    return jobs;
}

}  // namespace overlay
}  // namespace screen_capture
