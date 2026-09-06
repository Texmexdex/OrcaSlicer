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
#include <wx/image.h>
#include <wx/bitmap.h>

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

    wxImage m_original_img;
    wxImage m_quantized_img;
    bool    m_show_quantized{true};
};

class ImageTraceDialog : public wxDialog {
public:
    explicit ImageTraceDialog(wxWindow* parent);
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

    wxFilePickerCtrl*   m_file_picker{nullptr};
    wxDirPickerCtrl*    m_dir_picker{nullptr};
    wxSpinCtrl*         m_spin_k{nullptr};
    wxSpinCtrlDouble*   m_spin_width{nullptr};
    wxSpinCtrlDouble*   m_spin_height{nullptr};
    wxSpinCtrl*         m_spin_min_area{nullptr};
    wxButton*           m_btn_trace{nullptr};
    wxGrid*             m_grid{nullptr};

    ImagePreviewCanvas* m_preview_canvas{nullptr};
    wxRadioButton*      m_radio_quantized{nullptr};
    wxRadioButton*      m_radio_original{nullptr};
    wxStaticText*       m_lbl_preview_info{nullptr};

    std::vector<ColorTraceLayer> m_layers;
    std::vector<std::string>    m_exported_stl_paths;
};

} // namespace GUI
} // namespace Slic3r
