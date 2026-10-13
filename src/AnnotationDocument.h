#pragma once

#include <windows.h>

#include <array>
#include <string>
#include <vector>

namespace native_shot {

// 工具集合与 EShot 第一阶段的常用标注能力对应，但不携带任何 Qt 类型。
enum class Tool {
    None,
    Rectangle,
    Ellipse,
    Line,
    Arrow,
    Pen,
    Text,
    Counter,
    Mosaic,
};

// 标注坐标使用截图层客户区的物理像素；浮点数用于保留 Direct2D 绘制精度。
struct PointF {
    float x = 0.0F;
    float y = 0.0F;
};

// 一个完整标注。points 的含义由 tool 决定：图形保存起止点，画笔保存轨迹点，
// 文本和序号保存锚点；text/counterValue 只供对应工具使用。
struct Annotation {
    Tool tool = Tool::None;
    COLORREF color = RGB(255, 64, 64);
    float thickness = 3.0F;
    float fontSize = 24.0F;
    std::vector<PointF> points;
    std::wstring text;
    int counterValue = 0;
};

// 六个基础工具共用当前样式；提交时复制到标注，之后切换不会修改历史。
struct AnnotationStyle {
    inline static constexpr std::array<float, 3> widths{2.0F, 4.0F, 8.0F};
    inline static constexpr std::array<float, 3> fontSizes{18.0F, 24.0F, 36.0F};
    inline static constexpr std::array<COLORREF, 6> colors{
        RGB(16, 174, 239), RGB(145, 211, 0), RGB(255, 196, 0),
        RGB(75, 75, 75), RGB(255, 255, 255), RGB(255, 75, 80)};
    int sizeIndex = 1;
    int colorIndex = 5;

    static bool Supports(Tool tool) noexcept {
        return tool == Tool::Rectangle || tool == Tool::Ellipse ||
               tool == Tool::Line || tool == Tool::Arrow ||
               tool == Tool::Pen || tool == Tool::Text;
    }

    void Apply(Annotation& annotation) const {
        if (!Supports(annotation.tool)) return;
        annotation.thickness = widths.at(sizeIndex);
        annotation.fontSize = fontSizes.at(sizeIndex);
        annotation.color = colors.at(colorIndex);
    }
};

// 第一阶段的历史仅记录“追加一个标注”。这种模型已经覆盖绘制、撤销和重做，
// 等以后加入移动/缩放标注时，再把历史扩展成通用命令。
class AnnotationDocument {
public:
    // 添加新标注会形成新的历史分支，因此同时清空重做栈。
    void Add(Annotation annotation);
    // 在已提交标注栈与重做栈之间移动最后一个标注。
    bool Undo();
    bool Redo();
    void Clear();

    [[nodiscard]] bool CanUndo() const noexcept { return !m_items.empty(); }
    [[nodiscard]] bool CanRedo() const noexcept { return !m_redo.empty(); }
    [[nodiscard]] const std::vector<Annotation>& Items() const noexcept { return m_items; }

private:
    std::vector<Annotation> m_items;
    std::vector<Annotation> m_redo;
};

// 返回标注控制点的最小外接矩形，用于过滤零尺寸的草稿。
[[nodiscard]] RECT BoundsOf(const Annotation& annotation) noexcept;

}  // namespace native_shot
