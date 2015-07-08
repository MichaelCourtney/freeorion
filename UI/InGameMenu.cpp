#include "InGameMenu.h"

#include "ClientUI.h"
#include "CUIControls.h"
#include "OptionsWnd.h"
#include "SaveFileDialog.h"
#include "../client/human/HumanClientApp.h"
#include "../network/Networking.h"
#include "../util/i18n.h"
#include "../util/MultiplayerCommon.h"
#include "../util/OptionsDB.h"
#include "../util/Directories.h"
#include "../util/Logger.h"

#include <GG/Button.h>
#include <GG/Clr.h>
#include <GG/DrawUtil.h>
#include <GG/utf8/checked.h>

#include <boost/filesystem/operations.hpp>

namespace {
    const GG::X IN_GAME_OPTIONS_WIDTH(150);
    const GG::Y IN_GAME_OPTIONS_HEIGHT(280);
}

InGameMenu::InGameMenu():
    CUIWnd(UserString("GAME_MENU_WINDOW_TITLE"),
           (GG::GUI::GetGUI()->AppWidth() - IN_GAME_OPTIONS_WIDTH) / 2,
           (GG::GUI::GetGUI()->AppHeight() - IN_GAME_OPTIONS_HEIGHT) / 2,
           IN_GAME_OPTIONS_WIDTH, IN_GAME_OPTIONS_HEIGHT, GG::INTERACTIVE | GG::MODAL)
{
    m_save_btn = new CUIButton(UserString("GAME_MENU_SAVE"));
    m_load_btn = new CUIButton(UserString("GAME_MENU_LOAD"));
    m_options_btn = new CUIButton(UserString("INTRO_BTN_OPTIONS"));
    m_exit_btn = new CUIButton(UserString("GAME_MENU_RESIGN"));
    m_done_btn = new CUIButton(UserString("DONE"));

    AttachChild(m_save_btn);
    AttachChild(m_load_btn);
    AttachChild(m_options_btn);
    AttachChild(m_exit_btn);
    AttachChild(m_done_btn);

    GG::Connect(m_save_btn->LeftClickedSignal,      &InGameMenu::Save,      this);
    GG::Connect(m_load_btn->LeftClickedSignal,      &InGameMenu::Load,      this);
    GG::Connect(m_options_btn->LeftClickedSignal,   &InGameMenu::Options,   this);
    GG::Connect(m_exit_btn->LeftClickedSignal,      &InGameMenu::Exit,      this);
    GG::Connect(m_done_btn->LeftClickedSignal,      &InGameMenu::Done,      this);

    if (!HumanClientApp::GetApp()->SinglePlayerGame()) {
        // need lobby to load a multiplayer game; menu load of a file is insufficient
        m_load_btn->Disable();
    }

    if (!HumanClientApp::GetApp()->CanSaveNow()) {
        m_save_btn->Disable();
    }

    ValidatePosition();
    DoLayout();
}

InGameMenu::~InGameMenu()
{}

void InGameMenu::SizeMove(const GG::Pt& ul, const GG::Pt& lr) {
    // This wnd determines its own size.
    GG::Pt new_size(MinUsableSize());

    // This wnd determines its own position.
    GG::Pt new_ul(HumanClientApp::GetApp()->AppWidth()  * 0.5 - new_size.x/2,
                  HumanClientApp::GetApp()->AppHeight() * 0.5 - new_size.y/2);
    GG::Pt new_lr(HumanClientApp::GetApp()->AppWidth()  * 0.5 + new_size.x/2,
                  HumanClientApp::GetApp()->AppHeight() * 0.5 + new_size.y/2);

    CUIWnd::SizeMove(new_ul, new_lr);
}

GG::Pt InGameMenu::MinUsableSize() const {
    const GG::X H_MAINMENU_MARGIN(40);  //horizontal empty space
    const GG::Y V_MAINMENU_MARGIN(40);  //vertical empty space
    GG::X mainmenu_width(0);            //width of the mainmenu
    GG::Y mainmenu_height(0);           //height of the mainmenu

    // Calculate window width and height
    mainmenu_width  =        ButtonWidth()  + H_MAINMENU_MARGIN;
    mainmenu_height = 5.75 * ButtonCellHeight() + V_MAINMENU_MARGIN; // 8 rows + 0.75 before exit button

    return GG::Pt(mainmenu_width, mainmenu_height);
}

GG::X InGameMenu::ButtonWidth() const {
    const GG::X MIN_BUTTON_WIDTH(160);
    GG::X button_width(0);              //width of the buttons

    button_width = std::max(button_width, m_save_btn->MinUsableSize().x);
    button_width = std::max(button_width, m_load_btn->MinUsableSize().x);
    button_width = std::max(button_width, m_options_btn->MinUsableSize().x);
    button_width = std::max(button_width, m_exit_btn->MinUsableSize().x);
    button_width = std::max(button_width, m_done_btn->MinUsableSize().x);
    button_width = std::max(MIN_BUTTON_WIDTH, button_width);

    return button_width;
}

GG::Y InGameMenu::ButtonCellHeight() const {
    const GG::Y MIN_BUTTON_HEIGHT(40);
    return std::max(MIN_BUTTON_HEIGHT, m_done_btn->MinUsableSize().y);
}

void InGameMenu::DoLayout() {
    // place buttons
    GG::Pt button_ul(GG::X(15), GG::Y(12));
    GG::Pt button_lr(ButtonWidth(), m_done_btn->MinUsableSize().y);

    button_lr += button_ul;

    GG::Y button_cell_height = ButtonCellHeight();

    m_save_btn->SizeMove(button_ul, button_lr);
    button_ul.y += GG::Y(button_cell_height);
    button_lr.y += GG::Y(button_cell_height);
    m_load_btn->SizeMove(button_ul, button_lr);
    button_ul.y += GG::Y(button_cell_height);
    button_lr.y += GG::Y(button_cell_height);
    m_options_btn->SizeMove(button_ul, button_lr);
    button_ul.y += GG::Y(button_cell_height);
    button_lr.y += GG::Y(button_cell_height);
    m_exit_btn->SizeMove(button_ul, button_lr);
    button_ul.y += GG::Y(button_cell_height) * 1.75;
    button_lr.y += GG::Y(button_cell_height) * 1.75;
    m_done_btn->SizeMove(button_ul, button_lr);
}

void InGameMenu::KeyPress (GG::Key key, boost::uint32_t key_code_point,
                           GG::Flags<GG::ModKey> mod_keys)
{
    // Same behaviour as if "done" was pressed
    if (key == GG::GGK_RETURN || key == GG::GGK_KP_ENTER || key == GG::GGK_ESCAPE )
        Done();
}

void InGameMenu::Save() {
    DebugLogger() << "InGameMenu::Save";

    HumanClientApp* app = HumanClientApp::GetApp();
    if (!app)
        return;
    if (!app->CanSaveNow()) {
        ErrorLogger() << "InGameMenu::Save aborting; Client app can't save now";
        return;
    }

    const std::string SAVE_GAME_EXTENSION =
        app->SinglePlayerGame() ?
        SP_SAVE_FILE_EXTENSION : MP_SAVE_FILE_EXTENSION;

    try {
        std::string filename;

        // When saving in multiplayer, you cannot see the old saves or
        // browse directories, only give a save file name.
        if (app->SinglePlayerGame()) {
            DebugLogger() << "... running save file dialog";
            SaveFileDialog dlg(SAVE_GAME_EXTENSION);
            dlg.Run();
            filename = dlg.Result();
        } else {
            /// Multiplayer save. Talk to the server.
            filename = app->SelectSaveFile();
        }
        if (!filename.empty()) {
            if (!app->CanSaveNow()) {
                ErrorLogger() << "InGameMenu::Save aborting; Client app can't save now";
                throw std::runtime_error(UserString("UNABLE_TO_SAVE_NOW_TRY_AGAIN"));
            }
            DebugLogger() << "... initiating save to " << filename ;
            app->SaveGame(filename);
            CloseClicked();
            DebugLogger() << "... save done";
        }
    
    } catch (const std::exception& e) {
        ErrorLogger() << "Exception thrown attempting save: " << e.what();
        ClientUI::MessageBox(e.what(), true);
    }
}

void InGameMenu::Load() {
    Hide();
    HumanClientApp::GetApp()->LoadSinglePlayerGame();
    CloseClicked();
}

void InGameMenu::Options() {
    OptionsWnd options_wnd;
    options_wnd.Run();
}

void InGameMenu::Exit() {
    HumanClientApp::GetApp()->EndGame();
    CloseClicked();
}

void InGameMenu::Done()
{ m_done = true; }
