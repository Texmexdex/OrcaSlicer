#pragma once

#include "libslic3r/Geometry/ColorImageTracer.hpp"

#include <wx/dialog.h>
#include <wx/spinctrl.h>
#include <wx/filepicker.h>
#include <wx/button.h>
#include <wx/grid.h>
#include <wx/textctrl.h>
#include <wx/panel.h>
#include <wx/radiobut.h>
#include <wx/choice.h>
#include <wx/image.h>
#include <wx/bitmap.h>
#include <wx/colour.h>

#include <vector>
#include <string>

namespace Slic3r {
namespace GUI {

class ImagePreviewCanvas : public wxPanel {
public:
    explicit ImagePreviewCanvas(wxWindow* parent);
    ~ImagePreviewCanvas() override = default;

    void set_original_image(const wxImage& img);
    void set_quantized_image(const wxImage& img);
    void set_mode(bool show_quantized);
    void clear();

private:
    void on_paint(wxPaintEvent& evt);
    void on_size(wxSizeEvent& evt);
    void on_mouse_wheel(wxMouseEvent& evt);
    void on_mouse_left_down(wxMouseEvent& evt);
    void on_mouse_motion(wxMouseEvent& evt);
    void on_mouse_left_up(wxMouseEvent& evt);
    void on_mouse_dclick(wxMouseEvent& evt);

    wxImage m_original_img;
    wxImage m_quantized_img;
    bool    m_show_quantized{true};

    double  m_zoom{1.0};
    wxPoint m_pan_offset{0, 0};
    bool    m_is_dragging{false};
    wxPoint m_drag_start{0, 0};
};

class ImageTraceDialog : public wxDialog {
public:
    explicit ImageTraceDialog(wxWindow* parent, const std::vector<wxColour>& loaded_filaments = {});
    ~ImageTraceDialog() override = default;

    const std::vector<ColorTraceLayer>& get_layers() const { return m_layers; }
    const std::vector<std::string>& get_exported_stl_paths() const { return m_exported_stl_paths; }

private:
    void init_ui();
    void on_file_changed(wxFileDirPickerEvent& evt);
    void on_trace(wxCommandEvent& evt);
    void on_view_mode_changed(wxCommandEvent& evt);
    void on_export_only(wxCommandEvent& evt);
    void on_ok(wxCommandEvent& evt);
    bool save_stls();
    void sync_layers_from_grid();

    wxFilePickerCtrl*   m_file_picker{nullptr};
    wxDirPickerCtrl*    m_dir_picker{nullptr};
    wxSpinCtrl*         m_spin_k{nullptr};
    wxSpinCtrlDouble*   m_spin_width{nullptr};
    wxSpinCtrlDouble*   m_spin_height{nullptr};
    wxSpinCtrlDouble*   m_spin_smoothing{nullptr};
    wxSpinCtrl*         m_spin_min_area{nullptr};

    wxSpinCtrlDouble*   m_spin_default_offset{nullptr};
    wxChoice*           m_choice_default_corner{nullptr};
    wxChoice*           m_choice_default_face{nullptr};
    wxSpinCtrlDouble*   m_spin_default_face_h{nullptr};

    wxButton*           m_btn_trace{nullptr};
    wxGrid*             m_grid{nullptr};

    ImagePreviewCanvas* m_preview_canvas{nullptr};
    wxRadioButton*      m_radio_quantized{nullptr};
    wxRadioButton*      m_radio_original{nullptr};
    wxStaticText*       m_lbl_preview_info{nullptr};

    std::vector<wxColour>        m_loaded_filaments;
    std::vector<ColorTraceLayer>  m_layers;
    std::vector<std::string>     m_exported_stl_paths;
};

} // namespace GUI
} // namespace Slic3r
