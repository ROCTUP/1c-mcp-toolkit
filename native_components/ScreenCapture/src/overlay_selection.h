// Какие окна процесса дорисовываются поверх снимаемого окна и чем именно они снимаются.
//
// Правило вынесено из Win32-кода по той же причине, по которой popup_layer вынесен из COM в
// UIAutomation: правило, которое можно прогнать только против живой 1С, — это правило, чьи
// интересные случаи не проверяются никогда. Здесь нет ни одного вызова user32: на вход подаются
// плоские факты об окнах, на выход — упорядоченный список работ по отрисовке.
//
// Класс окна и его стили в правиле НЕ участвуют. Ключевание на "V8DropWindowExWnd" тихо перестанет
// работать при переименовании на новой платформе, и ни один тест этого не заметит; «окно моего
// процесса выше моего окна» переживает переименование.
#ifndef SCREEN_CAPTURE_OVERLAY_SELECTION_H
#define SCREEN_CAPTURE_OVERLAY_SELECTION_H

#include <vector>

namespace screen_capture {
namespace overlay {

// Окно-кандидат в том виде, в каком его отдаёт перечисление.
struct OverlayCandidate {
    unsigned long long hwnd = 0;
    unsigned long      pid = 0;   // из GetWindowThreadProcessId
    unsigned long      tid = 0;   // оттуда же: поток-владелец окна
    int  left = 0, top = 0;       // экранные координаты окна (GetWindowRect)
    int  width = 0, height = 0;
    bool visible = false;
    int  z = 0;                   // позиция в порядке перечисления: 0 — самое верхнее
};

// Кадр, в который вклеиваются слои.
struct CaptureFrame {
    unsigned long targetPid = 0;  // GetCurrentProcessId
    unsigned long selfTid = 0;    // GetCurrentThreadId в самой компоненте
    int originX = 0, originY = 0; // ClientToScreen(якорь, 0,0)
    int width = 0, height = 0;    // клиентская область якоря
    int anchorZ = 0;              // позиция якоря в том же порядке перечисления
    // Разрешён ли экран как источник пикселей. Пока false: экранный пиксель может содержать чужое
    // окно, а сказать об этом агенту ещё нечем. Становится true вместе с provenance-контрактом,
    // где у каждого слоя есть source.
    bool screenSourceAllowed = false;
};

enum class OverlayBackend {
    PrintWindow,  // окно нашего потока: SendMessage — прямой вызов оконной процедуры
    Screen,       // всё остальное: оконное сообщение в чужой поток не посылается никогда
};

// Одна работа по отрисовке.
struct OverlayPaint {
    unsigned long long hwnd = 0;
    int  srcLeft = 0, srcTop = 0; // откуда брать (экранные координаты)
    int  w = 0, h = 0;
    int  dstX = 0, dstY = 0;      // куда класть (клиентские координаты якоря, могут быть < 0)
    bool clipped = false;         // часть окна выходит за холст
    OverlayBackend backend = OverlayBackend::PrintWindow;
};

// Окна меньше этого по любой стороне считаются служебной мелочью и не рисуются.
const int kMinOverlaySide = 8;

// Порядок результата — снизу вверх: красить по нему подряд.
std::vector<OverlayPaint> SelectOverlays(const std::vector<OverlayCandidate>& all,
                                         const CaptureFrame& frame);

// Геометрия возвращаемого кадра. Композит идёт по полной клиентской области, а наружу может
// уехать только вырезанный region — поэтому отчёт о слоях обязан фильтроваться по тому
// прямоугольнику, который реально отдан, иначе метаданные описывают не ту картинку.
// Границы полуоткрытые: касание краем — не пересечение.
bool RectsIntersect(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh);
bool RectInside(int ax, int ay, int aw, int ah, int bx, int by, int bw, int bh);

}  // namespace overlay
}  // namespace screen_capture

#endif  // SCREEN_CAPTURE_OVERLAY_SELECTION_H
