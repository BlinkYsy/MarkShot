#include "AnnotationDocument.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace native_shot {

void AnnotationDocument::Add(Annotation annotation) {
    m_items.push_back(std::move(annotation));
    // 撤销后再绘制属于新分支，旧的重做记录不再有效。
    m_redo.clear();
}

bool AnnotationDocument::Undo() {
    if (m_items.empty()) {
        return false;
    }
    m_redo.push_back(std::move(m_items.back()));
    m_items.pop_back();
    return true;
}

bool AnnotationDocument::Redo() {
    if (m_redo.empty()) {
        return false;
    }
    m_items.push_back(std::move(m_redo.back()));
    m_redo.pop_back();
    return true;
}

void AnnotationDocument::Clear() {
    m_items.clear();
    m_redo.clear();
}

RECT BoundsOf(const Annotation& annotation) noexcept {
    if (annotation.points.empty()) {
        return {};
    }

    // 这里只计算控制点范围，不扩展线宽；调用方仅用它判断是否形成了形状。
    float left = annotation.points.front().x;
    float top = annotation.points.front().y;
    float right = left;
    float bottom = top;
    for (const PointF point : annotation.points) {
        left = std::min(left, point.x);
        top = std::min(top, point.y);
        right = std::max(right, point.x);
        bottom = std::max(bottom, point.y);
    }

    return {
        static_cast<LONG>(std::floor(left)),
        static_cast<LONG>(std::floor(top)),
        static_cast<LONG>(std::ceil(right)),
        static_cast<LONG>(std::ceil(bottom)),
    };
}

}  // namespace native_shot
