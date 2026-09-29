#include "curve_editor.h"

#include "brightness_curve.h"
#include "imgui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

namespace {

void AddTextRotated90CounterClockwise(
    ImDrawList* drawList,
    const ImVec2& targetCenter,
    ImU32 color,
    const char* text
) {
    if (!drawList || !text || text[0] == '\0') return;

    const int firstVertex = drawList->VtxBuffer.Size;
    drawList->AddText(ImVec2(0.0f, 0.0f), color, text);
    const int finalVertex = drawList->VtxBuffer.Size;
    if (finalVertex <= firstVertex) return;

    ImVec2 minimum = drawList->VtxBuffer[firstVertex].pos;
    ImVec2 maximum = minimum;
    for (int i = firstVertex; i < finalVertex; ++i) {
        const ImVec2 position = drawList->VtxBuffer[i].pos;
        minimum.x = (std::min)(minimum.x, position.x);
        minimum.y = (std::min)(minimum.y, position.y);
        maximum.x = (std::max)(maximum.x, position.x);
        maximum.y = (std::max)(maximum.y, position.y);
    }

    const ImVec2 sourceCenter(
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f);
    for (int i = firstVertex; i < finalVertex; ++i) {
        ImDrawVert& vertex = drawList->VtxBuffer[i];
        const float relativeX = vertex.pos.x - sourceCenter.x;
        const float relativeY = vertex.pos.y - sourceCenter.y;
        vertex.pos.x = targetCenter.x + relativeY;
        vertex.pos.y = targetCenter.y - relativeX;
    }
}

} // namespace

bool RenderCurveEditor(
    const char* id,
    std::vector<CurvePoint>& points,
    float currentLux
) {
    bool changed = false;
    if (points.size() < 2) {
        points = {{0.0f, 0.0f}, {MAX_VEML_LUX, 100.0f}};
        changed = true;
    }
    points.front().lux = 0.0f;
    points.front().brightness = 0.0f;
    points.back().brightness = 100.0f;

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    const ImVec2 originPos = ImGui::GetCursorScreenPos();
    constexpr float topMargin = 12.0f;
    constexpr float yMargin = 70.0f;
    constexpr float xMargin = 58.0f;
    constexpr float requestedSide = 572.0f;

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    float side = (std::min)(requestedSide, availableWidth - yMargin - 20.0f);
    side = (std::max)(side, 180.0f);

    const ImVec2 canvasPos(originPos.x + yMargin, originPos.y + topMargin);
    const ImVec2 canvasEnd(canvasPos.x + side, canvasPos.y + side);
    const ImVec2 totalSize(
        yMargin + side + 12.0f,
        topMargin + side + xMargin);
    ImGui::InvisibleButton(id, totalSize);

    ImGuiIO& io = ImGui::GetIO();
    const bool mouseInsidePlot =
        io.MousePos.x >= canvasPos.x &&
        io.MousePos.x <= canvasEnd.x &&
        io.MousePos.y >= canvasPos.y &&
        io.MousePos.y <= canvasEnd.y;
    ImGuiStorage* storage = ImGui::GetStateStorage();
    const ImGuiID xRangeId = ImGui::GetID((std::string(id) + "##XRange").c_str());
    float xAxisMaximum = storage->GetFloat(xRangeId, MAX_VEML_LUX);
    if (mouseInsidePlot && io.MouseWheel != 0.0f) {
        xAxisMaximum *= std::pow(0.8f, io.MouseWheel);
    }
    xAxisMaximum = (std::max)(
        10.0f, (std::min)(MAX_VEML_LUX, xAxisMaximum));
    storage->SetFloat(xRangeId, xAxisMaximum);

    drawList->AddRectFilled(
        canvasPos, canvasEnd, IM_COL32(30, 30, 34, 255), 4.0f);
    drawList->AddRect(
        canvasPos, canvasEnd, IM_COL32(70, 70, 78, 255), 4.0f);

    const auto toScreen = [&](const CurvePoint& point) {
        const float normalizedX = (std::max)(
            0.0f, point.lux / xAxisMaximum);
        const float normalizedY = (std::max)(
            0.0f, (std::min)(1.0f, point.brightness / 100.0f));
        return ImVec2(
            canvasPos.x + normalizedX * side,
            canvasEnd.y - normalizedY * side);
    };

    for (int brightness = 0; brightness <= 100; brightness += 25) {
        const float y = canvasEnd.y - (brightness / 100.0f) * side;
        if (brightness > 0 && brightness < 100) {
            drawList->AddLine(
                ImVec2(canvasPos.x, y),
                ImVec2(canvasEnd.x, y),
                IM_COL32(50, 50, 56, 180));
        }

        char valueText[16];
        std::snprintf(valueText, sizeof(valueText), "%d", brightness);
        const ImVec2 textSize = ImGui::CalcTextSize(valueText);
        drawList->AddText(
            ImVec2(canvasPos.x - textSize.x - 6.0f, y - textSize.y * 0.5f),
            IM_COL32(160, 160, 170, 255),
            valueText);
    }

    for (int tick = 0; tick <= 4; ++tick) {
        const float normalized = tick * 0.25f;
        const float x = canvasPos.x + normalized * side;
        if (tick > 0 && tick < 4) {
            drawList->AddLine(
                ImVec2(x, canvasPos.y),
                ImVec2(x, canvasEnd.y),
                IM_COL32(50, 50, 56, 180));
        }

        char valueText[24];
        const float tickValue = xAxisMaximum * normalized;
        if (xAxisMaximum >= MAX_VEML_LUX - 0.01f) {
            std::snprintf(valueText, sizeof(valueText), "%.2f", tickValue);
        } else if (xAxisMaximum < 100.0f) {
            std::snprintf(valueText, sizeof(valueText), "%.1f", tickValue);
        } else {
            std::snprintf(valueText, sizeof(valueText), "%.0f", tickValue);
        }
        const ImVec2 textSize = ImGui::CalcTextSize(valueText);
        drawList->AddText(
            ImVec2(x - textSize.x * 0.5f, canvasEnd.y + 5.0f),
            IM_COL32(160, 160, 170, 255),
            valueText);
    }

    const char* xAxisLabel = "Ambient";
    const ImVec2 xLabelSize = ImGui::CalcTextSize(xAxisLabel);
    drawList->AddText(
        ImVec2(
            canvasPos.x + (side - xLabelSize.x) * 0.5f,
            canvasEnd.y + 30.0f),
        IM_COL32(210, 210, 220, 255),
        xAxisLabel);
    AddTextRotated90CounterClockwise(
        drawList,
        ImVec2(originPos.x + 14.0f, canvasPos.y + side * 0.5f),
        IM_COL32(210, 210, 220, 255),
        "Screen");

    drawList->PushClipRect(canvasPos, canvasEnd, true);
    const ImVec2 firstPoint = toScreen(points.front());
    drawList->AddLine(
        ImVec2(canvasPos.x, firstPoint.y),
        firstPoint,
        IM_COL32(50, 160, 255, 255),
        2.0f);
    for (size_t i = 0; i + 1 < points.size(); ++i) {
        drawList->AddLine(
            toScreen(points[i]),
            toScreen(points[i + 1]),
            IM_COL32(50, 160, 255, 255),
            2.0f);
    }
    const ImVec2 finalPoint = toScreen(points.back());
    drawList->AddLine(
        finalPoint,
        ImVec2(canvasEnd.x, finalPoint.y),
        IM_COL32(50, 160, 255, 255),
        2.0f);

    if (g_SensorConnected.load(std::memory_order_relaxed) &&
        currentLux >= 0.0f &&
        currentLux <= xAxisMaximum) {
        const float displayedLux = currentLux;
        const float currentX =
            canvasPos.x + (displayedLux / xAxisMaximum) * side;
        drawList->AddLine(
            ImVec2(currentX, canvasPos.y),
            ImVec2(currentX, canvasEnd.y),
            IM_COL32(255, 180, 0, 180),
            1.5f);

        const float evaluatedBrightness =
            EvaluateBrightnessCurve(points, currentLux);
        const float currentY =
            canvasEnd.y - (evaluatedBrightness / 100.0f) * side;
        drawList->AddCircleFilled(
            ImVec2(currentX, currentY), 5.0f, IM_COL32(255, 200, 0, 255));
    }
    drawList->PopClipRect();

    static int draggingIndex = -1;
    static std::string activeEditorId;
    static bool dragChanged = false;
    int hoveredIndex = -1;

    for (size_t i = 0; i < points.size(); ++i) {
        const ImVec2 position = toScreen(points[i]);
        if (position.x < canvasPos.x || position.x > canvasEnd.x) continue;
        const bool isEndpoint = i == 0 || i == points.size() - 1;
        ImU32 color = isEndpoint
            ? IM_COL32(200, 200, 200, 255)
            : IM_COL32(0, 220, 120, 255);
        const bool hovered =
            std::hypot(
                io.MousePos.x - position.x,
                io.MousePos.y - position.y) <= 9.0f;

        if (hovered) {
            hoveredIndex = static_cast<int>(i);
            color = IM_COL32(255, 255, 0, 255);
            if (!isEndpoint &&
                ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                points.erase(points.begin() + static_cast<ptrdiff_t>(i));
                changed = true;
                hoveredIndex = -1;
                break;
            }
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                draggingIndex = static_cast<int>(i);
                activeEditorId = id;
                dragChanged = false;
            }
        }
        drawList->AddCircleFilled(position, isEndpoint ? 6.0f : 5.0f, color);
    }

    if (mouseInsidePlot &&
        hoveredIndex == -1 &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const float clickedLux =
            ((io.MousePos.x - canvasPos.x) / side) * xAxisMaximum;
        const float clickedBrightness =
            ((canvasEnd.y - io.MousePos.y) / side) * 100.0f;

        for (size_t i = 0; i + 1 < points.size(); ++i) {
            if (clickedLux < points[i].lux ||
                clickedLux > points[i + 1].lux) {
                continue;
            }
            if (points[i + 1].lux - points[i].lux > 5.0f) {
                const float validLux = (std::max)(
                    points[i].lux + 1.0f,
                    (std::min)(points[i + 1].lux - 1.0f, clickedLux));
                const float validBrightness = (std::max)(
                    points[i].brightness,
                    (std::min)(
                        points[i + 1].brightness, clickedBrightness));
                points.insert(
                    points.begin() + static_cast<ptrdiff_t>(i + 1),
                    {validLux, validBrightness});
                changed = true;
                draggingIndex = static_cast<int>(i + 1);
                activeEditorId = id;
                dragChanged = false;
            }
            break;
        }
    }

    if (draggingIndex >= 0 && activeEditorId == id) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const size_t index = static_cast<size_t>(draggingIndex);
            if (index < points.size()) {
                const CurvePoint oldPoint = points[index];
                float newLux =
                    ((io.MousePos.x - canvasPos.x) / side) * xAxisMaximum;
                float newBrightness =
                    ((canvasEnd.y - io.MousePos.y) / side) * 100.0f;

                if (index == 0) {
                    points[index].lux = 0.0f;
                    points[index].brightness = 0.0f;
                } else if (index == points.size() - 1) {
                    points[index].lux = (std::max)(
                        points[index - 1].lux + 0.5f,
                        (std::min)(MAX_VEML_LUX, newLux));
                    points[index].brightness = 100.0f;
                } else {
                    newLux = (std::max)(
                        points[index - 1].lux + 0.5f,
                        (std::min)(points[index + 1].lux - 0.5f, newLux));
                    newBrightness = (std::max)(
                        points[index - 1].brightness,
                        (std::min)(
                            points[index + 1].brightness, newBrightness));
                    points[index] = {newLux, newBrightness};
                }
                if (points[index].lux != oldPoint.lux ||
                    points[index].brightness != oldPoint.brightness) {
                    dragChanged = true;
                }
            }
        } else {
            changed = changed || dragChanged;
            draggingIndex = -1;
            activeEditorId.clear();
            dragChanged = false;
        }
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Mouse wheel: zoom horizontally.\n"
        "Left click: add a point.\nDrag: move a point.\n"
        "Right click: delete an interior point.");
    ImGui::Spacing();
    const std::string resetButtonId = "Reset##" + std::string(id);
    if (ImGui::Button(resetButtonId.c_str())) {
        points = {{0.0f, 0.0f}, {MAX_VEML_LUX, 100.0f}};
        changed = true;
    }
    return changed;
}
