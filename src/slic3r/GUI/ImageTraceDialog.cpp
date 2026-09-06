#include "ImageTraceDialog.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/msgdlg.h>
#include <wx/dcbuffer.h>
#include <wx/stdpaths.h>
#include <wx/filename.h>
#include <wx/generic/gridctrl.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {

// ----------------------------------------------------------------------------
// ImagePreviewCanvas Implementation (Interactive Zoom, Pan & Checkerboard)
// ----------------------------------------------------------------------------

ImagePreviewCanvas::ImagePreviewCanvas(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_THEME | wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    Bind(wxEVT_PAINT, &ImagePreviewCanvas::on_paint, this);
    Bind(wxEVT_SIZE, &ImagePreviewCanvas::on_size, this);
    Bind(wxEVT_ERASE_BACKGROUND, [](wxEraseEvent&) {}); // Prevent flicker
    Bind(wxEVT_MOUSEWHEEL, &ImagePreviewCanvas::on_mouse_wheel, this);
    Bind(wxEVT_LEFT_DOWN, &ImagePreviewCanvas::on_mouse_left_down, this);
    Bind(wxEVT_MOTION, &ImagePreviewCanvas::on_mouse_motion, this);
    Bind(wxEVT_LEFT_UP, &ImagePreviewCanvas::on_mouse_left_up, this);
    Bind(wxEVT_LEFT_DCLICK, &ImagePreviewCanvas::on_mouse_dclick, this);
}

void ImagePreviewCanvas::set_original_image(const wxImage& img)
{
    m_original_img = img;
    m_zoom = 1.0;
    m_pan_offset = wxPoint(0, 0);
    Refresh();
}

void ImagePreviewCanvas::set_quantized_image(const wxImage& img)
{
    m_quantized_img = img;
    Refresh();
}

void ImagePreviewCanvas::set_mode(bool show_quantized)
{
    m_show_quantized = show_quantized;
    Refresh();
}

void ImagePreviewCanvas::clear()
{
    m_original_img = wxImage();
    m_quantized_img = wxImage();
    m_zoom = 1.0;
    m_pan_offset = wxPoint(0, 0);
    m_is_dragging = false;
    Refresh();
}

void ImagePreviewCanvas::on_size(wxSizeEvent& evt)
{
    Refresh();
    evt.Skip();
}

void ImagePreviewCanvas::on_mouse_wheel(wxMouseEvent& evt)
{
    double factor = (evt.GetWheelRotation() > 0) ? 1.15 : (1.0 / 1.15);
    m_zoom = std::clamp(m_zoom * factor, 0.2, 25.0);
    Refresh();
}

void ImagePreviewCanvas::on_mouse_left_down(wxMouseEvent& evt)
{
    m_is_dragging = true;
    m_drag_start = evt.GetPosition();
    if (!HasCapture()) {
        CaptureMouse();
    }
}

void ImagePreviewCanvas::on_mouse_motion(wxMouseEvent& evt)
{
    if (m_is_dragging && evt.Dragging() && evt.LeftIsDown()) {
        wxPoint delta = evt.GetPosition() - m_drag_start;
        m_pan_offset += delta;
        m_drag_start = evt.GetPosition();
        Refresh();
    }
}

void ImagePreviewCanvas::on_mouse_left_up(wxMouseEvent&)
{
    if (m_is_dragging) {
        m_is_dragging = false;
        if (HasCapture()) {
            ReleaseMouse();
        }
    }
}

void ImagePreviewCanvas::on_mouse_dclick(wxMouseEvent&)
{
    m_zoom = 1.0;
    m_pan_offset = wxPoint(0, 0);
    Refresh();
}

void ImagePreviewCanvas::on_paint(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    wxSize sz = GetClientSize();
    if (sz.x <= 0 || sz.y <= 0) return;

    // Dark sleek canvas background
    dc.SetBackground(wxBrush(wxColour(28, 29, 33)));
    dc.Clear();

    const wxImage* active_img = nullptr;
    if (m_show_quantized && m_quantized_img.IsOk()) {
        active_img = &m_quantized_img;
    } else if (m_original_img.IsOk()) {
        active_img = &m_original_img;
    } else if (m_quantized_img.IsOk()) {
        active_img = &m_quantized_img;
    }

    if (active_img && active_img->IsOk() && active_img->GetWidth() > 0 && active_img->GetHeight() > 0) {
        int iw = active_img->GetWidth();
        int ih = active_img->GetHeight();
        int avail_w = sz.x - 24;
        int avail_h = sz.y - 24;

        if (avail_w > 0 && avail_h > 0) {
            double base_scale = std::min(static_cast<double>(avail_w) / iw, static_cast<double>(avail_h) / ih);
            double final_scale = base_scale * m_zoom;

            int tw = std::max(1, static_cast<int>(iw * final_scale));
            int th = std::max(1, static_cast<int>(ih * final_scale));

            int ox = (sz.x - tw) / 2 + m_pan_offset.x;
            int oy = (sz.y - th) / 2 + m_pan_offset.y;

            // Border outline around image
            dc.SetPen(wxPen(wxColour(65, 70, 80), 1));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRectangle(ox - 1, oy - 1, tw + 2, th + 2);

            wxImage scaled = active_img->Scale(tw, th, wxIMAGE_QUALITY_HIGH);
            wxBitmap bmp(scaled);
            dc.DrawBitmap(bmp, ox, oy, false);

            // Controls helper tooltip at bottom right
            dc.SetTextForeground(wxColour(110, 115, 125));
            wxFont hint_font = dc.GetFont();
            hint_font.SetPointSize(8);
            dc.SetFont(hint_font);
            wxString hint = _L("Wheel: Zoom  |  Drag: Pan  |  Double-Click: Reset View");
            wxCoord hw = 0, hh = 0;
            dc.GetTextExtent(hint, &hw, &hh);
            dc.DrawText(hint, sz.x - hw - 10, sz.y - hh - 6);
        }
    } else {
        // Placeholder instructions
        dc.SetTextForeground(wxColour(145, 150, 160));
        wxString msg = _L("Embedded Image Preview Window\n\n1. Select an image file (.png, .jpg, etc.)\n2. Set Tracing & Tolerance Offset parameters\n3. Click 'Trace & Preview'");
        wxCoord tw = 0, th = 0;
        dc.GetMultiLineTextExtent(msg, &tw, &th);
        dc.DrawText(msg, (sz.x - tw) / 2, (sz.y - th) / 2);
    }
}

// ----------------------------------------------------------------------------
// ImageTraceDialog Implementation
// ----------------------------------------------------------------------------

static wxString get_default_stl_dir()
{
    wxString desktop_dir = wxStandardPaths::Get().GetUserDir(wxStandardPaths::Dir_Desktop);
    if (desktop_dir.IsEmpty() || !wxDirExists(desktop_dir)) {
        desktop_dir = wxGetHomeDir() + "\\Desktop";
    }
    return desktop_dir + "\\Orca_Traced_STLs";
}

ImageTraceDialog::ImageTraceDialog(wxWindow* parent, const std::vector<wxColour>& loaded_filaments)
    : wxDialog(parent, wxID_ANY, _L("Native Image Vectorization & 3D Extrusion"),
               wxDefaultPosition, wxSize(1220, 780),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_loaded_filaments(loaded_filaments)
{
    init_ui();
    SetMinSize(wxSize(1020, 660));
}

void ImageTraceDialog::init_ui()
{
    auto* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Side-by-side layout: Left settings & layers, Right preview canvas
    auto* content_sizer = new wxBoxSizer(wxHORIZONTAL);

    // ==========================================
    // Left Pane (Parameters & Layer Grid)
    // ==========================================
    auto* left_sizer = new wxBoxSizer(wxVERTICAL);

    // 1. Parameter Settings Group
    auto* settings_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Tracing & Offset Parameters"));
    auto* grid_sizer = new wxFlexGridSizer(11, 2, 5, 10);
    grid_sizer->AddGrowableCol(1, 1);

    // Input Image File Selector
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Input Image:")), 0, wxALIGN_CENTER_VERTICAL);
    m_file_picker = new wxFilePickerCtrl(
        settings_box->GetStaticBox(), wxID_ANY, wxEmptyString,
        _L("Select raster image to trace"),
        _L("Image Files (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp"),
        wxDefaultPosition, wxDefaultSize,
        wxFLP_OPEN | wxFLP_FILE_MUST_EXIST | wxFLP_USE_TEXTCTRL);
    m_file_picker->Bind(wxEVT_FILEPICKER_CHANGED, &ImageTraceDialog::on_file_changed, this);
    grid_sizer->Add(m_file_picker, 1, wxEXPAND);

    // Color Cluster Count (K)
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Color Clusters (K):")), 0, wxALIGN_CENTER_VERTICAL);
    m_spin_k = new wxSpinCtrl(settings_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 2, 16, 4);
    grid_sizer->Add(m_spin_k, 0, wxEXPAND);

    // Bed Target Width (mm)
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Bed Target Width (mm):")), 0, wxALIGN_CENTER_VERTICAL);
    m_spin_width = new wxSpinCtrlDouble(settings_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1.0, 1000.0, 100.0, 1.0);
    grid_sizer->Add(m_spin_width, 0, wxEXPAND);

    // Default Base Height (mm)
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Base Height (mm):")), 0, wxALIGN_CENTER_VERTICAL);
    m_spin_height = new wxSpinCtrlDouble(settings_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0.1, 200.0, 2.0, 0.2);
    grid_sizer->Add(m_spin_height, 0, wxEXPAND);

    // Curve Smoothing (px)
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Curve Smoothing (px):")), 0, wxALIGN_CENTER_VERTICAL);
    m_spin_smoothing = new wxSpinCtrlDouble(settings_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0.0, 5.0, 0.8, 0.1);
    m_spin_smoothing->SetToolTip(_L("Douglas-Peucker contour simplification tolerance (0.8 recommended; 0 = exact pixel steps)"));
    grid_sizer->Add(m_spin_smoothing, 0, wxEXPAND);

    // Minimum Area Filter (pixels)
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Min Area Filter (px):")), 0, wxALIGN_CENTER_VERTICAL);
    m_spin_min_area = new wxSpinCtrl(settings_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 1, 100000, 5);
    m_spin_min_area->SetToolTip(_L("Minimum pixel area to preserve fine details (default 5 preserves small text & pupils)"));
    grid_sizer->Add(m_spin_min_area, 0, wxEXPAND);

    // Tolerance Offset (XY Inset/Outset)
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Default Offset (mm):")), 0, wxALIGN_CENTER_VERTICAL);
    m_spin_default_offset = new wxSpinCtrlDouble(settings_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, -5.0, 5.0, 0.0, 0.05);
    m_spin_default_offset->SetToolTip(_L("XY tolerance offset: Negative for inlay/fit clearance gap (e.g. -0.15mm), Positive for perimeter overlap choke"));
    grid_sizer->Add(m_spin_default_offset, 0, wxEXPAND);

    // Corner Style
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Default Corner Style:")), 0, wxALIGN_CENTER_VERTICAL);
    wxArrayString corner_opts;
    corner_opts.Add(_L("Sharp (Miter)"));
    corner_opts.Add(_L("Round (Fillet)"));
    corner_opts.Add(_L("Beveled (Chamfer)"));
    m_choice_default_corner = new wxChoice(settings_box->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, corner_opts);
    m_choice_default_corner->SetSelection(0);
    grid_sizer->Add(m_choice_default_corner, 0, wxEXPAND);

    // Face Profile
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Default Face Profile:")), 0, wxALIGN_CENTER_VERTICAL);
    wxArrayString face_opts;
    face_opts.Add(_L("Flat (Planar)"));
    face_opts.Add(_L("Chamfer (Beveled Shoulder)"));
    face_opts.Add(_L("Fillet (Rounded Shoulder)"));
    face_opts.Add(_L("Peaked (Pyramid Roof)"));
    face_opts.Add(_L("Bubbled (Inflated Dome)"));
    m_choice_default_face = new wxChoice(settings_box->GetStaticBox(), wxID_ANY, wxDefaultPosition, wxDefaultSize, face_opts);
    m_choice_default_face->SetSelection(0);
    grid_sizer->Add(m_choice_default_face, 0, wxEXPAND);

    // Face Feature Height (mm)
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Face Contour H (mm):")), 0, wxALIGN_CENTER_VERTICAL);
    m_spin_default_face_h = new wxSpinCtrlDouble(settings_box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxSP_ARROW_KEYS, 0.1, 50.0, 0.8, 0.1);
    m_spin_default_face_h->SetToolTip(_L("Height or depth of chamfer, fillet, pyramid peak, or dome curve"));
    grid_sizer->Add(m_spin_default_face_h, 0, wxEXPAND);

    // STL Export Directory
    grid_sizer->Add(new wxStaticText(settings_box->GetStaticBox(), wxID_ANY, _L("Save STLs To:")), 0, wxALIGN_CENTER_VERTICAL);
    m_dir_picker = new wxDirPickerCtrl(
        settings_box->GetStaticBox(), wxID_ANY, get_default_stl_dir(),
        _L("Select directory to save STL component files"),
        wxDefaultPosition, wxDefaultSize,
        wxDIRP_DIR_MUST_EXIST | wxDIRP_USE_TEXTCTRL);
    grid_sizer->Add(m_dir_picker, 1, wxEXPAND);

    settings_box->Add(grid_sizer, 0, wxEXPAND | wxALL, 6);

    // Action button to trigger trace
    m_btn_trace = new wxButton(settings_box->GetStaticBox(), wxID_ANY, _L("Trace & Preview"));
    m_btn_trace->Bind(wxEVT_BUTTON, &ImageTraceDialog::on_trace, this);
    settings_box->Add(m_btn_trace, 0, wxALIGN_RIGHT | wxALL, 6);

    left_sizer->Add(settings_box, 0, wxEXPAND | wxALL, 6);

    // 2. Detected Layers Grid (9 Columns with full parametric offset & face control)
    auto* layers_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Detected Color Layers (Per-Layer Parameters)"));

    m_grid = new wxGrid(layers_box->GetStaticBox(), wxID_ANY);
    m_grid->CreateGrid(0, 9);
    m_grid->SetColLabelValue(0, _L("Swatch"));
    m_grid->SetColLabelValue(1, _L("Extruder"));
    m_grid->SetColLabelValue(2, _L("Height"));
    m_grid->SetColLabelValue(3, _L("Offset (mm)"));
    m_grid->SetColLabelValue(4, _L("Corners"));
    m_grid->SetColLabelValue(5, _L("Face"));
    m_grid->SetColLabelValue(6, _L("Face H"));
    m_grid->SetColLabelValue(7, _L("Negative"));
    m_grid->SetColLabelValue(8, _L("Active"));

    m_grid->SetColSize(0, 60);
    m_grid->SetColSize(1, 60);
    m_grid->SetColSize(2, 65);
    m_grid->SetColSize(3, 75);
    m_grid->SetColSize(4, 75);
    m_grid->SetColSize(5, 75);
    m_grid->SetColSize(6, 65);
    m_grid->SetColSize(7, 65);
    m_grid->SetColSize(8, 50);

    layers_box->Add(m_grid, 1, wxEXPAND | wxALL, 4);
    left_sizer->Add(layers_box, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 6);

    content_sizer->Add(left_sizer, 0, wxEXPAND | wxRIGHT, 6);

    // ==========================================
    // Right Pane (Dedicated Canvas View Window)
    // ==========================================
    auto* preview_box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Canvas Preview (Interactive View Window)"));

    // Preview Mode Header Toolbar
    auto* header_sizer = new wxBoxSizer(wxHORIZONTAL);
    m_radio_quantized = new wxRadioButton(preview_box->GetStaticBox(), wxID_ANY, _L("Quantized Colors"), wxDefaultPosition, wxDefaultSize, wxRB_GROUP);
    m_radio_original  = new wxRadioButton(preview_box->GetStaticBox(), wxID_ANY, _L("Original Image"));
    m_radio_quantized->SetValue(true);

    m_radio_quantized->Bind(wxEVT_RADIOBUTTON, &ImageTraceDialog::on_view_mode_changed, this);
    m_radio_original->Bind(wxEVT_RADIOBUTTON, &ImageTraceDialog::on_view_mode_changed, this);

    header_sizer->Add(m_radio_quantized, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);
    header_sizer->Add(m_radio_original, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 15);

    m_lbl_preview_info = new wxStaticText(preview_box->GetStaticBox(), wxID_ANY, _L("No image loaded"));
    m_lbl_preview_info->SetForegroundColour(wxColour(120, 125, 135));
    header_sizer->Add(m_lbl_preview_info, 1, wxALIGN_CENTER_VERTICAL);

    preview_box->Add(header_sizer, 0, wxEXPAND | wxALL, 6);

    // Embedded Preview Canvas
    m_preview_canvas = new ImagePreviewCanvas(preview_box->GetStaticBox());
    preview_box->Add(m_preview_canvas, 1, wxEXPAND | wxALL, 4);

    content_sizer->Add(preview_box, 1, wxEXPAND | wxLEFT, 6);

    main_sizer->Add(content_sizer, 1, wxEXPAND | wxALL, 8);

    // ==========================================
    // 3. Dialog Bottom Buttons (Export / OK / Cancel)
    // ==========================================
    auto* bottom_sizer = new wxBoxSizer(wxHORIZONTAL);

    auto* btn_export_only = new wxButton(this, wxID_ANY, _L("Export STLs Only"));
    btn_export_only->Bind(wxEVT_BUTTON, &ImageTraceDialog::on_export_only, this);
    bottom_sizer->Add(btn_export_only, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, 10);

    bottom_sizer->AddStretchSpacer(1);

    auto* ok_btn = new wxButton(this, wxID_OK, _L("Save STLs & Import to Plate"));
    ok_btn->SetDefault();
    auto* cancel_btn = new wxButton(this, wxID_CANCEL, _L("Cancel"));

    bottom_sizer->Add(ok_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 6);
    bottom_sizer->Add(cancel_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    ok_btn->Bind(wxEVT_BUTTON, &ImageTraceDialog::on_ok, this);

    main_sizer->Add(bottom_sizer, 0, wxEXPAND | wxALL, 8);

    SetSizer(main_sizer);
    Layout();
    CentreOnParent();
}

void ImageTraceDialog::on_file_changed(wxFileDirPickerEvent&)
{
    wxString path = m_file_picker->GetPath();
    if (path.IsEmpty() || !wxFileExists(path)) {
        m_preview_canvas->clear();
        m_lbl_preview_info->SetLabel(_L("No image loaded"));
        return;
    }

    wxImage img;
    if (img.LoadFile(path)) {
        m_preview_canvas->set_original_image(img);
        m_preview_canvas->set_mode(false);
        m_radio_original->SetValue(true);
        m_lbl_preview_info->SetLabel(wxString::Format(_L("Original Image: %d x %d px"), img.GetWidth(), img.GetHeight()));
    }
}

void ImageTraceDialog::on_view_mode_changed(wxCommandEvent&)
{
    bool show_quantized = m_radio_quantized->GetValue();
    m_preview_canvas->set_mode(show_quantized);
}

void ImageTraceDialog::on_trace(wxCommandEvent&)
{
    wxString path = m_file_picker->GetPath();
    if (path.IsEmpty() || !wxFileExists(path)) {
        wxMessageBox(_L("Please select a valid raster image file."), _L("File Error"), wxOK | wxICON_WARNING, this);
        return;
    }

    int k_clusters = m_spin_k->GetValue();
    double width_mm = m_spin_width->GetValue();
    int min_area_px = m_spin_min_area->GetValue();
    double default_height = m_spin_height->GetValue();
    double smooth_px = m_spin_smoothing->GetValue();
    double default_offset = m_spin_default_offset->GetValue();
    CornerStyle default_corner = static_cast<CornerStyle>(m_choice_default_corner->GetSelection());
    FaceProfile default_face = static_cast<FaceProfile>(m_choice_default_face->GetSelection());
    double default_face_h = m_spin_default_face_h->GetValue();

    wxBusyCursor wait;
    std::vector<unsigned char> preview_rgb;
    int preview_w = 0;
    int preview_h = 0;

    bool success = ColorImageTracer::process_image(
        std::string(path.ToUTF8()),
        k_clusters,
        width_mm,
        min_area_px,
        smooth_px,
        m_layers,
        &preview_rgb,
        &preview_w,
        &preview_h);

    if (!success || m_layers.empty()) {
        wxMessageBox(_L("Failed to trace contours from image. Check image format and parameters."),
                     _L("Trace Error"), wxOK | wxICON_ERROR, this);
        return;
    }

    // Update preview canvas with quantized segmentation
    if (preview_w > 0 && preview_h > 0 && !preview_rgb.empty()) {
        wxImage q_img(preview_w, preview_h);
        memcpy(q_img.GetData(), preview_rgb.data(), static_cast<size_t>(preview_w) * preview_h * 3);
        m_preview_canvas->set_quantized_image(q_img);
        m_preview_canvas->set_mode(true);
        m_radio_quantized->SetValue(true);
        m_lbl_preview_info->SetLabel(wxString::Format(_L("Quantized (%d layers): %d x %d px"),
            static_cast<int>(m_layers.size()), preview_w, preview_h));
    }

    // Automatically match detected color layers to nearest loaded extruder filament color
    if (!m_loaded_filaments.empty()) {
        for (auto& layer : m_layers) {
            double min_dist_sq = 1e9;
            int best_idx = 0;
            for (size_t f = 0; f < m_loaded_filaments.size(); ++f) {
                const wxColour& fc = m_loaded_filaments[f];
                double dr = static_cast<double>(layer.r) - fc.Red();
                double dg = static_cast<double>(layer.g) - fc.Green();
                double db = static_cast<double>(layer.b) - fc.Blue();
                double dist_sq = dr * dr + dg * dg + db * db;
                if (dist_sq < min_dist_sq) {
                    min_dist_sq = dist_sq;
                    best_idx = static_cast<int>(f);
                }
            }
            layer.extruder_id = best_idx + 1;
        }
    }

    // Apply default parameters to all layers and extrude initial meshes
    for (auto& layer : m_layers) {
        layer.height_mm = default_height;
        layer.offset_mm = default_offset;
        layer.corner_style = default_corner;
        layer.face_profile = default_face;
        layer.face_height_mm = default_face_h;
        ColorImageTracer::extrude_layer(layer);
    }

    // Populate wxGrid
    if (m_grid->GetNumberRows() > 0) {
        m_grid->DeleteRows(0, m_grid->GetNumberRows());
    }

    m_grid->AppendRows(static_cast<int>(m_layers.size()));

    wxArrayString corner_choices;
    corner_choices.Add("Sharp");
    corner_choices.Add("Round");
    corner_choices.Add("Bevel");

    wxArrayString face_choices;
    face_choices.Add("Flat");
    face_choices.Add("Chamfer");
    face_choices.Add("Fillet");
    face_choices.Add("Peaked");
    face_choices.Add("Bubbled");

    for (int i = 0; i < static_cast<int>(m_layers.size()); ++i) {
        const auto& layer = m_layers[i];

        // Column 0: Visual Color Swatch
        m_grid->SetCellValue(i, 0, wxEmptyString);
        m_grid->SetCellBackgroundColour(i, 0, wxColour(layer.r, layer.g, layer.b));
        m_grid->SetReadOnly(i, 0, true);

        // Column 1: Extruder Assignment (1 to 16)
        m_grid->SetCellEditor(i, 1, new wxGridCellNumberEditor(1, 16));
        m_grid->SetCellRenderer(i, 1, new wxGridCellNumberRenderer());
        m_grid->SetCellValue(i, 1, wxString::Format("%d", layer.extruder_id));

        // Column 2: Layer Extrusion Height (mm)
        m_grid->SetCellEditor(i, 2, new wxGridCellFloatEditor(4, 2));
        m_grid->SetCellRenderer(i, 2, new wxGridCellFloatRenderer(4, 2));
        m_grid->SetCellValue(i, 2, wxString::Format("%.2f", layer.height_mm));

        // Column 3: Tolerance Offset (mm)
        m_grid->SetCellEditor(i, 3, new wxGridCellFloatEditor(4, 2));
        m_grid->SetCellRenderer(i, 3, new wxGridCellFloatRenderer(4, 2));
        m_grid->SetCellValue(i, 3, wxString::Format("%.2f", layer.offset_mm));

        // Column 4: Corner Style (Sharp, Round, Bevel)
        m_grid->SetCellEditor(i, 4, new wxGridCellChoiceEditor(corner_choices));
        m_grid->SetCellValue(i, 4, (layer.corner_style == CornerStyle::Round) ? "Round" :
                                   (layer.corner_style == CornerStyle::Beveled) ? "Bevel" : "Sharp");

        // Column 5: Face Profile (Flat, Chamfer, Fillet, Peaked, Bubbled)
        m_grid->SetCellEditor(i, 5, new wxGridCellChoiceEditor(face_choices));
        wxString face_str = "Flat";
        if (layer.face_profile == FaceProfile::Chamfer) face_str = "Chamfer";
        else if (layer.face_profile == FaceProfile::Fillet) face_str = "Fillet";
        else if (layer.face_profile == FaceProfile::Peaked) face_str = "Peaked";
        else if (layer.face_profile == FaceProfile::Bubbled) face_str = "Bubbled";
        m_grid->SetCellValue(i, 5, face_str);

        // Column 6: Face Contour Height (mm)
        m_grid->SetCellEditor(i, 6, new wxGridCellFloatEditor(4, 2));
        m_grid->SetCellRenderer(i, 6, new wxGridCellFloatRenderer(4, 2));
        m_grid->SetCellValue(i, 6, wxString::Format("%.2f", layer.face_height_mm));

        // Column 7: Part Type ("Part" vs "Negative Volume / Cutter")
        m_grid->SetCellEditor(i, 7, new wxGridCellBoolEditor());
        m_grid->SetCellRenderer(i, 7, new wxGridCellBoolRenderer());
        m_grid->SetCellValue(i, 7, layer.is_negative ? "1" : "0");

        // Column 8: Enabled/Active checkbox
        m_grid->SetCellEditor(i, 8, new wxGridCellBoolEditor());
        m_grid->SetCellRenderer(i, 8, new wxGridCellBoolRenderer());
        m_grid->SetCellValue(i, 8, "1");
    }

    m_grid->Refresh();
}

void ImageTraceDialog::sync_layers_from_grid()
{
    if (m_grid->IsCellEditControlEnabled()) {
        m_grid->DisableCellEditControl();
    }
    m_grid->SaveEditControlValue();

    for (int i = 0; i < static_cast<int>(m_layers.size()) && i < m_grid->GetNumberRows(); ++i) {
        long ext_id = 1;
        m_grid->GetCellValue(i, 1).ToLong(&ext_id);
        m_layers[i].extruder_id = std::clamp(static_cast<int>(ext_id), 1, 16);

        double h = 2.0;
        m_grid->GetCellValue(i, 2).ToDouble(&h);
        if (h <= 0.0) h = 0.2;

        double offset = 0.0;
        m_grid->GetCellValue(i, 3).ToDouble(&offset);

        wxString corner_str = m_grid->GetCellValue(i, 4).Lower();
        CornerStyle corner = CornerStyle::Sharp;
        if (corner_str.Contains("round")) corner = CornerStyle::Round;
        else if (corner_str.Contains("bevel")) corner = CornerStyle::Beveled;

        wxString face_str = m_grid->GetCellValue(i, 5).Lower();
        FaceProfile face = FaceProfile::Flat;
        if (face_str.Contains("chamfer")) face = FaceProfile::Chamfer;
        else if (face_str.Contains("fillet")) face = FaceProfile::Fillet;
        else if (face_str.Contains("peak")) face = FaceProfile::Peaked;
        else if (face_str.Contains("bubble")) face = FaceProfile::Bubbled;

        double face_h = 0.8;
        m_grid->GetCellValue(i, 6).ToDouble(&face_h);

        wxString neg_val = m_grid->GetCellValue(i, 7);
        m_layers[i].is_negative = (neg_val == "1" || neg_val.Lower() == "true");

        bool geom_changed = (std::abs(m_layers[i].height_mm - h) > 0.001 ||
                             std::abs(m_layers[i].offset_mm - offset) > 0.001 ||
                             m_layers[i].corner_style != corner ||
                             m_layers[i].face_profile != face ||
                             std::abs(m_layers[i].face_height_mm - face_h) > 0.001);

        m_layers[i].height_mm = h;
        m_layers[i].offset_mm = offset;
        m_layers[i].corner_style = corner;
        m_layers[i].face_profile = face;
        m_layers[i].face_height_mm = face_h;

        if (geom_changed || m_layers[i].mesh.empty()) {
            ColorImageTracer::extrude_layer(m_layers[i]);
        }
    }
}

bool ImageTraceDialog::save_stls()
{
    if (m_layers.empty()) return false;

    wxString base_dir = m_dir_picker->GetPath();
    if (base_dir.IsEmpty()) {
        base_dir = get_default_stl_dir();
    }

    if (!wxDirExists(base_dir)) {
        wxFileName::Mkdir(base_dir, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }

    wxFileName src_fn(m_file_picker->GetPath());
    wxString img_name = src_fn.GetName();
    if (img_name.IsEmpty()) {
        img_name = "Traced_Image";
    }

    wxString img_folder = base_dir + "\\" + img_name;
    if (!wxDirExists(img_folder)) {
        wxFileName::Mkdir(img_folder, wxS_DIR_DEFAULT, wxPATH_MKDIR_FULL);
    }

    m_exported_stl_paths.clear();

    for (size_t i = 0; i < m_layers.size(); ++i) {
        if (m_layers[i].mesh.empty()) continue;

        wxString filename = wxString::Format("%s_part_%d_rgb%02X%02X%02X.stl",
            img_name,
            static_cast<int>(i + 1),
            m_layers[i].r, m_layers[i].g, m_layers[i].b);
        wxString fullpath = img_folder + "\\" + filename;

        std::string path_u8 = std::string(fullpath.ToUTF8());
        if (its_write_stl_binary(path_u8.c_str(), "", m_layers[i].mesh)) {
            m_exported_stl_paths.push_back(path_u8);
        }
    }

    return !m_exported_stl_paths.empty();
}

void ImageTraceDialog::on_export_only(wxCommandEvent&)
{
    if (m_layers.empty() || m_grid->GetNumberRows() == 0) {
        wxMessageBox(_L("No layers detected. Please trace an image first."),
                     _L("Validation Error"), wxOK | wxICON_WARNING, this);
        return;
    }

    sync_layers_from_grid();

    // Filter to enabled/active layers only
    std::vector<ColorTraceLayer> active_layers;
    for (int i = 0; i < static_cast<int>(m_layers.size()) && i < m_grid->GetNumberRows(); ++i) {
        wxString active_val = m_grid->GetCellValue(i, 8);
        bool is_active = (active_val == "1" || active_val.Lower() == "true");
        if (is_active && !m_layers[i].mesh.empty()) {
            active_layers.push_back(m_layers[i]);
        }
    }

    if (active_layers.empty()) {
        wxMessageBox(_L("Please enable at least one active layer to export."),
                     _L("Validation Error"), wxOK | wxICON_WARNING, this);
        return;
    }

    m_layers = std::move(active_layers);

    if (save_stls()) {
        wxString folder = wxPathOnly(m_exported_stl_paths.front());
        wxMessageBox(wxString::Format(_L("Successfully exported %d STL component files to:\n%s"),
                                      static_cast<int>(m_exported_stl_paths.size()), folder),
                     _L("Export Successful"), wxOK | wxICON_INFORMATION, this);
    } else {
        wxMessageBox(_L("Failed to save STL files to disk. Check folder write permissions."),
                     _L("Export Error"), wxOK | wxICON_ERROR, this);
    }
}

void ImageTraceDialog::on_ok(wxCommandEvent&)
{
    if (m_layers.empty() || m_grid->GetNumberRows() == 0) {
        wxMessageBox(_L("No layers detected. Please trace an image first."),
                     _L("Validation Error"), wxOK | wxICON_WARNING, this);
        return;
    }

    sync_layers_from_grid();

    // Filter to enabled/active layers only
    std::vector<ColorTraceLayer> active_layers;
    for (int i = 0; i < static_cast<int>(m_layers.size()) && i < m_grid->GetNumberRows(); ++i) {
        wxString active_val = m_grid->GetCellValue(i, 8);
        bool is_active = (active_val == "1" || active_val.Lower() == "true");
        if (is_active && !m_layers[i].mesh.empty()) {
            active_layers.push_back(m_layers[i]);
        }
    }

    if (active_layers.empty()) {
        wxMessageBox(_L("Please enable at least one active layer to add to the build plate."),
                     _L("Validation Error"), wxOK | wxICON_WARNING, this);
        return;
    }

    m_layers = std::move(active_layers);

    if (!save_stls()) {
        wxMessageBox(_L("Failed to save STL files to disk. Check folder write permissions."),
                     _L("Export Error"), wxOK | wxICON_ERROR, this);
        return;
    }

    EndModal(wxID_OK);
}

} // namespace GUI
} // namespace Slic3r
