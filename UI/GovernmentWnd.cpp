#include "GovernmentWnd.h"

#include "ClientUI.h"
#include "CUIControls.h"
#include "QueueListBox.h"   // for PromptRow
#include "IconTextBrowseWnd.h"
#include "Sound.h"
#include "TextBrowseWnd.h"
#include "SidePanel.h"
#include "FleetWnd.h"
#include "../parse/Parse.h"
#include "../util/i18n.h"
#include "../util/Logger.h"
#include "../util/Order.h"
#include "../util/OptionsDB.h"
#include "../util/ScopedTimer.h"
#include "../Empire/Empire.h"
#include "../Empire/Government.h"
#include "../client/human/GGHumanClientApp.h"

#include <GG/DrawUtil.h>
#include <GG/StaticGraphic.h>
#include <GG/TabWnd.h>

#include <boost/cast.hpp>
#include <boost/algorithm/string.hpp>

using boost::placeholders::_1;
using boost::placeholders::_2;

enum class Availability : size_t {
    Adopted = 0,      // Policy is currently adopted
    Adoptable = 1,    // Policy can be adopted, but hasn't been adopted
    Unaffordable = 2, // Policy is unlocked and has no restrictions, but is too expensive to adopt
    Restricted = 3,   // Policy is unlocked, but is restricted by exclusions and thus cannot be adopted
    Locked = 4        // Policy has not been unlocked and thus cannot be adopted
};

namespace {
    constexpr std::string_view  POLICY_CONTROL_DROP_TYPE_STRING = "Policy Control";
    const std::string           EMPTY_STRING;
    constexpr GG::X             POLICY_CONTROL_WIDTH{120};
    constexpr GG::Y             POLICY_CONTROL_HEIGHT{180};
    constexpr GG::X             SLOT_CONTROL_WIDTH{120};
    constexpr GG::Y             SLOT_CONTROL_HEIGHT{180};
    constexpr int               PAD{3};
    constexpr double            POLICY_PAD{0.125};
    constexpr double            POLICY_TEXT_POS_X{0.0625};
    constexpr double            POLICY_TEXT_POS_Y{0.75};
    constexpr double            POLICY_COST_POS_X{0.75};
    constexpr double            POLICY_COST_POS_Y{0.0625};
    constexpr GG::X             POLICY_SIZE_BUTTON_WIDTH{32};

    /** Returns texture with which to render a PolicySlotControl, depending on
      * \a category */
    std::shared_ptr<GG::Texture> SlotBackgroundTexture(const std::string& category) {
        std::string filename = boost::algorithm::to_lower_copy(category) + "_slot.png";
        return ClientUI::GetTexture(ClientUI::ArtDir() / "icons" / "policies" / filename, true);
    }

    /** Returns background texture with which to render a PolicyControl,
      * depending on the category of slot that the indicated \a policy can
      * be put into. */
    std::shared_ptr<GG::Texture> PolicyBackgroundTexture(const Policy* policy) {
        if (policy) {
            std::string filename = boost::algorithm::to_lower_copy(policy->Category()) + "_policy.png";
            return ClientUI::GetTexture(ClientUI::ArtDir() / "icons" / "policies" / filename, true);
        }
        return ClientUI::GetTexture(ClientUI::ArtDir() / "misc" / "missing.png", true);
    }

    //////////////////////////////////////////////////
    //  AvailabilityManager                         //
    //////////////////////////////////////////////////
    /** A class to allow the storage of the state of a GUI availabilty filter
        and the querying of that state WRT a policy. */
    class AvailabilityManager {
    public:
        using DisplayedAvailabilies = std::array<bool, 5>;

        AvailabilityManager(bool adopted, bool adoptable, bool unaffordable, bool restricted, bool locked);

        const DisplayedAvailabilies& GetAvailabilities() const { return m_availabilities; };
        bool GetAvailability(const Availability type) const;
        void SetAvailability(const Availability type, const bool state);
        void ToggleAvailability(const Availability type);

        /** Given the GUI's displayed availabilities as stored in this
            AvailabilityManager, return the displayed state of the \p policy.
            Return none if the \p policy should not be displayed. */
        bool PolicyDisplayed(const Policy* policy, int empire_id = GGHumanClientApp::GetApp()->EmpireID()) const;

    private:
        // A tuple of the toogle state of the 3-tuple of coupled
        // availability filters in the GUI:
        // Restricted, Available and Unavailable
        DisplayedAvailabilies m_availabilities;
    };

    AvailabilityManager::AvailabilityManager(bool adopted, bool adoptable, bool unaffordable,
                                             bool restricted, bool locked) :
        m_availabilities{adopted, adoptable, unaffordable, restricted, locked}
    {}

    bool AvailabilityManager::GetAvailability(Availability availability) const {
        auto idx = static_cast<std::underlying_type_t<Availability>>(availability);
        if (idx > m_availabilities.size())
            return false;
        return m_availabilities[idx];
    }

    void AvailabilityManager::SetAvailability(const Availability availability, bool state) {
        auto idx = static_cast<std::underlying_type_t<Availability>>(availability);
        if (idx > m_availabilities.size())
            ErrorLogger() << "AvailabilityManager::SetAvailability passed invalid availability: " << idx;
        else
            m_availabilities[idx] = state;
    }

    void AvailabilityManager::ToggleAvailability(const Availability type)
    { SetAvailability(type, !GetAvailability(type)); }

    bool AvailabilityManager::PolicyDisplayed(const Policy* policy, int empire_id) const {
        if (!policy)
            return false;
        const auto [show_adopted, show_adoptable, show_unaffordable, show_restricted, show_locked] = m_availabilities;

        ScriptingContext context;
        auto empire = context.GetEmpire(empire_id);
        if (!empire)
            return true;
        context.source = empire->Source(context.ContextObjects());

        bool policy_adopted = empire->PolicyAdopted(policy->Name());
        bool policy_affordable = empire->PolicyAffordable(policy->Name(), context);
        bool policy_restricted = !empire->PolicyPrereqsAndExclusionsOK(policy->Name());
        bool policy_locked = !empire->PolicyAvailable(policy->Name());

        if (policy_adopted && show_adopted)
            return true;

        if (show_locked && policy_locked && !policy_adopted)
            return true;

        if (show_unaffordable && !policy_affordable && !policy_adopted && !policy_restricted && !policy_locked)
            return true;

        if (show_restricted && policy_restricted && !policy_adopted && !policy_locked)
            return true;

        if (show_adoptable && !policy_adopted && policy_affordable && !policy_restricted && !policy_locked)
            return true;

        return false;
    }

    //////////////////////////
    //    PolicyBrowseWnd   //
    //////////////////////////
    std::shared_ptr<GG::BrowseInfoWnd> PolicyBrowseWnd(const std::string& policy_name) {
        const Policy* policy = GetPolicy(policy_name);
        if (!policy)
            return nullptr;

        const ScriptingContext context;
        int empire_id = GGHumanClientApp::GetApp()->EmpireID();
        auto empire = context.GetEmpire(empire_id);

        std::string main_text;
        main_text += UserString(policy->Category()) + " - ";
        main_text += UserString(policy->ShortDescription()) + "\n\n";

        if (empire) {
            bool available = empire->PolicyAvailable(policy_name);
            bool adopted = empire->PolicyAdopted(policy_name);
            //bool affordable = empire->PolicyAffordable(policy_name, context);
            auto cost = policy->AdoptionCost(empire_id, context);

            const auto& adoption_cost_template{adopted ?
                UserString("POLICY_ADOPTED") : available ?
                UserString("POLICY_ADOPTABLE_COST") : UserString("POLICY_LOCKED")};
            main_text += boost::io::str(FlexibleFormat(adoption_cost_template) % cost) + "\n\n";

            auto current_adoption_duration = empire->CurrentTurnsPolicyHasBeenAdopted(policy_name);
            auto total_adoption_duration = empire->CumulativeTurnsPolicyHasBeenAdopted(policy_name);
            const auto& adopted_time_timeplate{adopted ?
                UserString("POLICY_ADOPTION_TIMES_CURRENT_AND_TOTAL") : UserString("POLICY_ADOPTION_TIME_TOTAL")};
            main_text += boost::io::str(FlexibleFormat(adopted_time_timeplate)
                                        % current_adoption_duration % total_adoption_duration) + "\n\n";
        }

        main_text += UserString(policy->Description());

        return GG::Wnd::Create<IconTextBrowseWnd>(
            ClientUI::PolicyIcon(policy_name), UserString(policy_name), main_text);
    }
}


//////////////////////////////////////////////////
// PolicyControl                                //
//////////////////////////////////////////////////
/** UI representation of a government policy.  Displayed in the PolicyPalette,
  * and can be dragged onto SlotControls to add policies to the government. */
class PolicyControl : public GG::Control {
public:
    explicit PolicyControl(const Policy* policy);
    void CompleteConstruction() override;

    const std::string&  PolicyName() const  { return m_policy ? m_policy->Name() : EMPTY_STRING; }
    const Policy*       GetPolicy() const   { return m_policy; }

    void Resize(const GG::Pt& sz, const int pts = ClientUI::Pts());
    void Render() override;

    void LClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) override;
    void LDoubleClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) override;
    void RClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) override;

    mutable boost::signals2::signal<void (const Policy*, GG::Flags<GG::ModKey>)> ClickedSignal;
    mutable boost::signals2::signal<void (const Policy*, const GG::Pt& pt)> RightClickedSignal;
    mutable boost::signals2::signal<void (const Policy*)> DoubleClickedSignal;

private:
    std::shared_ptr<GG::TextControl>    m_name_label;
    std::shared_ptr<GG::TextControl>    m_cost_label;
    std::shared_ptr<GG::StaticGraphic>  m_icon;
    std::shared_ptr<GG::StaticGraphic>  m_background;
    const Policy*                       m_policy = nullptr;
};

PolicyControl::PolicyControl(const Policy* policy) :
    GG::Control(GG::X0, GG::Y0, SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT, GG::INTERACTIVE),
    m_policy(policy)
{}

void PolicyControl::CompleteConstruction() {
    GG::Control::CompleteConstruction();
    if (!m_policy)
        return;
    int empire_id = GGHumanClientApp::GetApp()->EmpireID();
    const ScriptingContext context;
    const auto& name = UserString(m_policy->Name());
    const auto cost = static_cast<int>(m_policy->AdoptionCost(empire_id, context));

    //std::cout << "PolicyControl: " << m_policy->Name() << std::endl;

    m_background = GG::Wnd::Create<GG::StaticGraphic>(PolicyBackgroundTexture(m_policy),
                                                      GG::GRAPHIC_FITGRAPHIC | GG::GRAPHIC_PROPSCALE);
    m_icon = GG::Wnd::Create<GG::StaticGraphic>(ClientUI::PolicyIcon(m_policy->Name()),
                                                      GG::GRAPHIC_FITGRAPHIC | GG::GRAPHIC_PROPSCALE);
    m_name_label = GG::Wnd::Create<GG::TextControl>(GG::X0, GG::Y0, GG::X1, GG::Y1, name,
                                                      ClientUI::GetBoldFont(), ClientUI::TextColor(), GG::FORMAT_WORDBREAK);
    m_cost_label = GG::Wnd::Create<GG::TextControl>(GG::X0, GG::Y0, GG::X1, GG::Y1, std::to_string(cost),
                                                      ClientUI::GetBoldFont(), ClientUI::TextColor());

    m_background->Show();
    m_icon->Show();

    AttachChild(m_background);
    AttachChild(m_icon);
    AttachChild(m_name_label);
    AttachChild(m_cost_label);

    Resize(Size());

    //DebugLogger() << "PolicyControl::PolicyControl this: " << this << " policy: " << policy << " named: " << (policy ? policy->Name() : "no policy");
    SetDragDropDataType(POLICY_CONTROL_DROP_TYPE_STRING);

    //DebugLogger() << "PolicyControl::PolicyControl policy name: " << m_policy->Name();
    SetBrowseModeTime(GetOptionsDB().Get<int>("ui.tooltip.delay"));
    SetBrowseInfoWnd(PolicyBrowseWnd(m_policy->Name()));
}

void PolicyControl::Resize(const GG::Pt& sz, const int pts) {
    m_background->Resize(sz);
    m_icon->Resize(GG::Pt(sz.x, sz.y * 2/3));

    std::shared_ptr<GG::Font> font = ClientUI::GetBoldFont(pts);

    m_name_label->SetFont(font);
    m_cost_label->SetFont(std::move(font));

    m_name_label->SizeMove(GG::Pt(sz.x * POLICY_TEXT_POS_X, sz.y * POLICY_TEXT_POS_Y),
                           GG::Pt(sz.x * (1 - POLICY_TEXT_POS_X), sz.y * POLICY_TEXT_POS_Y));
    m_cost_label->MoveTo(GG::Pt(sz.x * POLICY_COST_POS_X, sz.y * (1 - POLICY_COST_POS_Y)));

    GG::Control::Resize(sz);
}

void PolicyControl::Render() {}

void PolicyControl::LClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys)
{ ClickedSignal(m_policy, mod_keys); }

void PolicyControl::LDoubleClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys)
{ DoubleClickedSignal(m_policy); }

void PolicyControl::RClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys)
{ RightClickedSignal(m_policy, pt); }


//////////////////////////////////////////////////
// PoliciesListBox                              //
//////////////////////////////////////////////////
/** Arrangement of PolicyControls that can be dragged onto SlotControls */
class PoliciesListBox : public CUIListBox {
public:
    class PoliciesListBoxRow : public CUIListBox::Row {
    public:
        PoliciesListBoxRow(GG::X w, GG::Y h, const AvailabilityManager& availabilities_state);
        void ChildrenDraggedAway(const std::vector<GG::Wnd*>& wnds,
                                 const GG::Wnd* destination) override;
    private:
        const AvailabilityManager& m_availabilities_state;
    };

    explicit PoliciesListBox(const AvailabilityManager& availabilities_state);

    const std::set<std::string>&    GetCategoriesShown() const;
    const AvailabilityManager&      AvailabilityState() const { return m_availabilities_state; }

    void SizeMove(const GG::Pt& ul, const GG::Pt& lr) override;
    void AcceptDrops(const GG::Pt& pt, std::vector<std::shared_ptr<GG::Wnd>> wnds,
                     GG::Flags<GG::ModKey> mod_keys) override;
    void Populate();

    void ShowCategory(const std::string& category, bool refresh_list = true);
    void ShowAllCategories(bool refresh_list = true);
    void HideCategory(const std::string& category, bool refresh_list = true);
    void HideAllCategories(bool refresh_list = true);
    void ResizePolicies(const GG::Pt& sz, const int pts = ClientUI::Pts());

    mutable boost::signals2::signal<void (const Policy*, GG::Flags<GG::ModKey>)>    PolicyClickedSignal;
    mutable boost::signals2::signal<void (const Policy*)>                           PolicyDoubleClickedSignal;
    mutable boost::signals2::signal<void (const Policy*, const GG::Pt& pt)>         PolicyRightClickedSignal;
    mutable boost::signals2::signal<void (const std::string&)>                      ClearPolicySignal;

protected:
    void DropsAcceptable(DropsAcceptableIter first, DropsAcceptableIter last,
                         const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) const override;

private:
    // policies indexed by category that conform to current availability state
    std::map<std::string, std::vector<const Policy*>>
    GroupAvailableDisplayablePolicies(const Empire* empire) const;

    mutable boost::signals2::connection m_empire_policies_changed_signal_connection;
    std::set<std::string>               m_policy_categories_shown;
    int                                 m_previous_num_columns = -1;
    const AvailabilityManager&          m_availabilities_state;
};

PoliciesListBox::PoliciesListBoxRow::PoliciesListBoxRow(
    GG::X w, GG::Y h, const AvailabilityManager& availabilities_state) :
    CUIListBox::Row(w, h),
    m_availabilities_state(availabilities_state)
{}

void PoliciesListBox::PoliciesListBoxRow::ChildrenDraggedAway(
    const std::vector<GG::Wnd*>& wnds, const GG::Wnd* destination)
{
    if (wnds.empty())
        return;
    // should only be one wnd in list because PolicyControls doesn't allow selection, so dragging is
    // only one-at-a-time
    auto control = dynamic_cast<GG::Control*>(wnds.front());
    if (!control || empty())
        return;

    // find control in row
    unsigned int ii = 0;
    for (; ii < size(); ++ii) {
        if (at(ii) != control)
            continue;
    }

    if (ii == size())
        return;

    RemoveCell(ii);  // Wnd that accepts drop takes ownership of dragged-away control

    auto policy_control = dynamic_cast<PolicyControl*>(control);
    if (!policy_control)
        return;

    const auto policy_type = policy_control->GetPolicy();
    if (!policy_type)
        return;

    auto new_policy_control = GG::Wnd::Create<PolicyControl>(policy_type);
    if (auto parent = std::dynamic_pointer_cast<const PoliciesListBox>(Parent())) {
        new_policy_control->ClickedSignal.connect(parent->PolicyClickedSignal);
        new_policy_control->DoubleClickedSignal.connect(parent->PolicyDoubleClickedSignal);
        new_policy_control->RightClickedSignal.connect(parent->PolicyRightClickedSignal);
    }

    SetCell(ii, new_policy_control);
}

PoliciesListBox::PoliciesListBox(const AvailabilityManager& availabilities_state) :
    CUIListBox(),
    m_availabilities_state(availabilities_state)
{
    ManuallyManageColProps();
    NormalizeRowsOnInsert(false);
    SetStyle(GG::LIST_NOSEL);
}

const std::set<std::string>& PoliciesListBox::GetCategoriesShown() const
{ return m_policy_categories_shown; }

void PoliciesListBox::SizeMove(const GG::Pt& ul, const GG::Pt& lr) {
    GG::Pt old_size = GG::Wnd::Size();

    auto policy_palette = Parent();
    auto gov_wnd = std::dynamic_pointer_cast<GovernmentWnd>(policy_palette->Parent());

    GG::Pt slot_size = GG::Pt(SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT);
    if (gov_wnd)
        slot_size = gov_wnd->GetPolicySlotSize();

    // maybe later do something interesting with docking
    CUIListBox::SizeMove(ul, lr);

    if (old_size != GG::Wnd::Size()) {
        // determine how many columns can fit in the box now...
        const GG::X TOTAL_WIDTH = Size().x - ClientUI::ScrollWidth();
        const int NUM_COLUMNS = std::max(1,
            Value(TOTAL_WIDTH / (slot_size.x + GG::X(PAD))));

        if (NUM_COLUMNS != m_previous_num_columns)
            Populate();
    }
}

void PoliciesListBox::AcceptDrops(const GG::Pt& pt,
                                  std::vector<std::shared_ptr<GG::Wnd>> wnds,
                                  GG::Flags<GG::ModKey> mod_keys)
{
    // If ctrl is pressed then signal all policies of the same type to be cleared.
    if (!(GG::GUI::GetGUI()->ModKeys() & GG::MOD_KEY_CTRL))
        return;

    if (wnds.empty())
        return;

    auto control = std::dynamic_pointer_cast<const PolicyControl>(*wnds.begin());
    const Policy* policy_type = control ? control->GetPolicy() : nullptr;
    if (!policy_type)
        return;

    ClearPolicySignal(policy_type->Name());
}

std::map<std::string, std::vector<const Policy*>>
PoliciesListBox::GroupAvailableDisplayablePolicies(const Empire* empire) const {
    std::map<std::string, std::vector<const Policy*>> policies_categorized;

    // loop through all possible policies
    for (auto& [policy_name, policy] : GetPolicyManager()) {
        (void)policy_name; // quiet warning
        const auto& category = policy->Category();

        // check whether this policy should be shown in list
        if (!m_policy_categories_shown.count(category))
            continue;   // policies of this category are not requested to be shown

        // Check if part satisfies availability
        if (m_availabilities_state.PolicyDisplayed(policy.get()))
            policies_categorized[category].push_back(policy.get());
    }
    return policies_categorized;
}

void PoliciesListBox::Populate() {
    //std::cout << "PoliciesListBox::Populate" << std::endl;
    ScopedTimer scoped_timer("PoliciesListBox::Populate");

    GG::Pt slot_size = GG::Pt(SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT);
    int text_pts = ClientUI::Pts();

    auto policy_palette = Parent();
    auto gov_wnd = std::dynamic_pointer_cast<GovernmentWnd>(policy_palette->Parent());
    if (gov_wnd) {
        slot_size = gov_wnd->GetPolicySlotSize();
        text_pts = gov_wnd->GetPolicyTextSize();
    }

    const GG::X TOTAL_WIDTH = ClientWidth() - ClientUI::ScrollWidth();
    const int MAX_COLUMNS = std::max(1, Value(TOTAL_WIDTH / (slot_size.x + GG::X(PAD))));

    int empire_id = GGHumanClientApp::GetApp()->EmpireID();
    const Empire* empire = GetEmpire(empire_id);  // may be nullptr

    m_empire_policies_changed_signal_connection.disconnect();
    if (empire)
        m_empire_policies_changed_signal_connection = empire->PoliciesChangedSignal.connect(
            boost::bind(&PoliciesListBox::Populate, this), boost::signals2::at_front);

    int cur_col = MAX_COLUMNS;
    std::shared_ptr<PoliciesListBoxRow> cur_row;
    int num_policies = 0;

    // remove policies currently in rows of listbox
    Clear();

    // filter policies by availability and current designation of categories
    // for display
    for (auto& [category_name, policies_vec] : GroupAvailableDisplayablePolicies(empire)) {
        (void)category_name; // quiet warning
        // take the sorted policies and make UI element rows for the PoliciesListBox
        for (const auto policy : policies_vec) {
            // check if current row is full, and make a new row if necessary
            if (cur_col >= MAX_COLUMNS) {
                if (cur_row)
                    Insert(cur_row);
                cur_col = 0;
                cur_row = GG::Wnd::Create<PoliciesListBoxRow>(
                    TOTAL_WIDTH, slot_size.y + GG::Y(PAD), m_availabilities_state);
            }
            ++cur_col;
            ++num_policies;

            // make new policy control and add to row
            auto control = GG::Wnd::Create<PolicyControl>(policy);
            control->ClickedSignal.connect(PoliciesListBox::PolicyClickedSignal);
            control->DoubleClickedSignal.connect(PoliciesListBox::PolicyDoubleClickedSignal);
            control->RightClickedSignal.connect(PoliciesListBox::PolicyRightClickedSignal);

            cur_row->push_back(std::move(control));
        }
    }
    // add any incomplete rows
    if (cur_row)
        Insert(std::move(cur_row));

    // keep track of how many columns are present now
    m_previous_num_columns = MAX_COLUMNS;

    // If there are no policies add a prompt to suggest a solution.
    if (num_policies == 0)
        Insert(GG::Wnd::Create<PromptRow>(TOTAL_WIDTH, UserString("ALL_POLICY_AVAILABILITY_FILTERS_BLOCKING_PROMPT")),
               begin(), false);
}

void PoliciesListBox::ShowCategory(const std::string& category, bool refresh_list) {
    if (!m_policy_categories_shown.count(category)) {
        m_policy_categories_shown.insert(category);
        if (refresh_list)
            Populate();
    }
}

void PoliciesListBox::ShowAllCategories(bool refresh_list) {
    auto cats = GetPolicyManager().PolicyCategories();
    std::transform(cats.begin(), cats.end(), std::inserter(m_policy_categories_shown, m_policy_categories_shown.end()),
                   [](const auto sv) { return std::string{sv}; });
    if (refresh_list)
        Populate();
}

void PoliciesListBox::HideCategory(const std::string& category, bool refresh_list) {
    if (m_policy_categories_shown.erase(category) > 0 && refresh_list)
        Populate();
}

void PoliciesListBox::HideAllCategories(bool refresh_list) {
    m_policy_categories_shown.clear();
    if (refresh_list)
        Populate();
}

void PoliciesListBox::ResizePolicies(const GG::Pt& sz, const int pts) {
    auto it = begin();
    while (it != end()) {
        auto row = *it;
        unsigned int itt = 0;
        while (itt < row->size()) {
            auto policy_control = dynamic_cast<PolicyControl*>(row->at(itt));
            if (policy_control)
                policy_control->Resize(sz, pts);
            ++itt;
        }
        ++it;
    }
}

//////////////////////////////////////////////////
// GovernmentWnd::PolicyPalette                 //
//////////////////////////////////////////////////
/** Contains graphical list of PolicyControl which can be dragged and dropped
  * onto slots to assign policies to those slots */
class GovernmentWnd::PolicyPalette : public GG::Wnd {
public:
    PolicyPalette(GG::X w, GG::Y h);
    void CompleteConstruction() override;

    void SizeMove(const GG::Pt& ul, const GG::Pt& lr) override;

    void ShowCategory(const std::string& category, bool refresh_list = true);
    void ShowAllCategories(bool refresh_list = true);
    void HideCategory(const std::string& category, bool refresh_list = true);
    void HideAllCategories(bool refresh_list = true);
    void ToggleCategory(const std::string& category, bool refresh_list = true);
    void ToggleAllCategories(bool refresh_list = true);

    void ToggleAvailability(const Availability type);

    void Populate();

    mutable boost::signals2::signal<void (const Policy*, GG::Flags<GG::ModKey>)> PolicyClickedSignal;
    mutable boost::signals2::signal<void (const Policy*)> PolicyDoubleClickedSignal;
    mutable boost::signals2::signal<void (const Policy*, const GG::Pt& pt)> PolicyRightClickedSignal;
    mutable boost::signals2::signal<void (const std::string&)> ClearPolicySignal;

private:
    void DoLayout();

    /** A policy type click with ctrl obsoletes policy. */
    void HandlePolicyClicked(const Policy*, GG::Flags<GG::ModKey>);
    void HandlePolicyRightClicked(const Policy*, const GG::Pt& pt);

    std::shared_ptr<PoliciesListBox>                        m_policies_list;
    std::map<std::string, std::shared_ptr<CUIStateButton>>  m_category_buttons;
    AvailabilityManager                                     m_availabilities_state{true, true, false, false, false};
    static constexpr size_t NUM_AVAILABILITY_BUTTONS = 5;
    std::array<std::shared_ptr<CUIStateButton>, NUM_AVAILABILITY_BUTTONS> m_availabilities_buttons{};
};

GovernmentWnd::PolicyPalette::PolicyPalette(GG::X w, GG::Y h) :
    GG::Wnd(GG::X0, GG::Y0, w, h, GG::ONTOP | GG::INTERACTIVE)
{}

void GovernmentWnd::PolicyPalette::CompleteConstruction() {
    SetChildClippingMode(ChildClippingMode::ClipToClient);

    m_policies_list = GG::Wnd::Create<PoliciesListBox>(m_availabilities_state);
    AttachChild(m_policies_list);
    m_policies_list->PolicyClickedSignal.connect(
        boost::bind(&GovernmentWnd::PolicyPalette::HandlePolicyClicked, this, _1, _2));
    m_policies_list->PolicyDoubleClickedSignal.connect(
        PolicyDoubleClickedSignal);
    m_policies_list->PolicyRightClickedSignal.connect(
        boost::bind(&GovernmentWnd::PolicyPalette::HandlePolicyRightClicked, this, _1, _2));
    m_policies_list->ClearPolicySignal.connect(ClearPolicySignal);

    // class buttons
    for (auto& cat_view : GetPolicyManager().PolicyCategories()) {
        // are there any policies of this class?
        if (std::none_of(GetPolicyManager().begin(), GetPolicyManager().end(),
                         [cat_view](auto& e){ return e.second && cat_view == e.second->Category(); }))
        { continue; }

        const auto& us_cateory{UserString(cat_view)};
        auto [ptr_it, ignored] = m_category_buttons.emplace(std::string{cat_view}, GG::Wnd::Create<CUIStateButton>(
            us_cateory, GG::FORMAT_CENTER, std::make_shared<CUILabelButtonRepresenter>()));
        (void)ignored;
        auto& [string_in_map, ptr_in_map] = *ptr_it;
        AttachChild(ptr_in_map);
        ptr_in_map->CheckedSignal.connect(
            boost::bind(&GovernmentWnd::PolicyPalette::ToggleCategory, this, string_in_map, true));
    }

    constexpr std::array<std::string_view, 5> BUTTON_LABELS = {{
            UserStringNop("POLICY_LIST_ADOPTED"), UserStringNop("POLICY_LIST_ADOPTABLE"),
            UserStringNop("POLICY_LIST_UNAFFORDABLE"), UserStringNop("POLICY_LIST_RESTRICTED"),
            UserStringNop("POLICY_LIST_LOCKED")}};
    for (size_t n = 0; n < 5; ++n) {
        Availability avail = static_cast<Availability>(n);
        auto& button = m_availabilities_buttons[n];
        button = GG::Wnd::Create<CUIStateButton>(UserString(BUTTON_LABELS[n]),
                                                 GG::FORMAT_CENTER, std::make_shared<CUILabelButtonRepresenter>());
        AttachChild(button);
        button->CheckedSignal.connect(
            boost::bind(&GovernmentWnd::PolicyPalette::ToggleAvailability, this, Availability(n)));
        button->SetCheck(m_availabilities_state.GetAvailability(Availability(n)));
    }

    // default to showing everything
    ShowAllCategories(false);
    Populate();

    GG::Wnd::CompleteConstruction();
}

void GovernmentWnd::PolicyPalette::SizeMove(const GG::Pt& ul, const GG::Pt& lr) {
    GG::Wnd::SizeMove(ul, lr);
    DoLayout();
}

void GovernmentWnd::PolicyPalette::DoLayout() {
    const int     PTS = ClientUI::Pts();
    const GG::X   PTS_WIDE{PTS/2};        // guess at how wide per character the font needs
    const GG::Y   BUTTON_HEIGHT{PTS*3/2};
    constexpr int BUTTON_SEPARATION = 3;  // vertical or horizontal sepration between adjacent buttons
    constexpr int BUTTON_EDGE_PAD = 2;    // distance from edges of control to buttons
    constexpr GG::X RIGHT_EDGE_PAD{8};    // to account for border of CUIWnd

    const GG::X USABLE_WIDTH = std::max(ClientWidth() - RIGHT_EDGE_PAD, GG::X1);// space in which to fit buttons
    constexpr int GUESSTIMATE_NUM_CHARS_IN_BUTTON_LABEL = 14; // rough guesstimate... avoid overly long policy class names
    const GG::X MIN_BUTTON_WIDTH = PTS_WIDE*GUESSTIMATE_NUM_CHARS_IN_BUTTON_LABEL;
    const size_t MAX_BUTTONS_PER_ROW = std::max(Value(USABLE_WIDTH / (MIN_BUTTON_WIDTH + BUTTON_SEPARATION)), 1);

    const size_t NUM_CATEGORY_BUTTONS = std::max(1, static_cast<int>(m_category_buttons.size()));
    const size_t TOTAL_BUTTONS = NUM_CATEGORY_BUTTONS + NUM_AVAILABILITY_BUTTONS;

    // determine how to arrange category and availability buttons into rows and columns
    // eg:
    // [CAT1] [CAT2] [CAT3] [AVB1] [AVB2] [AVB3] [AVB4] [AVB5]
    //
    // [CAT1] [CAT2] [AVB1] [AVB2] [AVB3]
    // [CAT3]        [AVB4] [AVB5]
    //
    // [CAT1] [AVB1] [AVB2]
    // [CAT2] [AVB3] [AVB4]
    // [CAT3] [AVB5]

    float NUM_ROWS = 1.0f;
    size_t AVAILABILITY_BUTTONS_PER_ROW = static_cast<size_t>(std::ceil(NUM_AVAILABILITY_BUTTONS / NUM_ROWS));
    size_t CATEGORY_BUTTONS_PER_ROW = static_cast<size_t>(std::ceil(NUM_CATEGORY_BUTTONS / NUM_ROWS));
    int TOTAL_BUTTONS_PER_ROW = AVAILABILITY_BUTTONS_PER_ROW + CATEGORY_BUTTONS_PER_ROW;
    while (TOTAL_BUTTONS_PER_ROW > static_cast<int>(MAX_BUTTONS_PER_ROW) && NUM_ROWS < TOTAL_BUTTONS) {
        NUM_ROWS += 1.0f;
        AVAILABILITY_BUTTONS_PER_ROW = static_cast<size_t>(std::ceil(NUM_AVAILABILITY_BUTTONS / NUM_ROWS));
        CATEGORY_BUTTONS_PER_ROW = static_cast<size_t>(std::ceil(NUM_CATEGORY_BUTTONS / NUM_ROWS));
        TOTAL_BUTTONS_PER_ROW = AVAILABILITY_BUTTONS_PER_ROW + CATEGORY_BUTTONS_PER_ROW;
    }

    //const size_t NUM_CATEGORY_BUTTON_ROWS = static_cast<size_t>(std::ceil(
    //    NUM_CATEGORY_BUTTONS * 1.0f / CATEGORY_BUTTONS_PER_ROW));
    //const size_t NUM_AVAILABILITY_BUTTON_ROWS = static_cast<size_t>(std::ceil(
    //    NUM_AVAILABILITY_BUTTONS * 1.0f / AVAILABILITY_BUTTONS_PER_ROW));

    const GG::X BUTTON_WIDTH = (USABLE_WIDTH - (TOTAL_BUTTONS_PER_ROW - 1)*BUTTON_SEPARATION) / TOTAL_BUTTONS_PER_ROW;

    const GG::X COL_OFFSET = BUTTON_WIDTH + BUTTON_SEPARATION;    // horizontal distance between each column of buttons
    const GG::Y ROW_OFFSET = BUTTON_HEIGHT + BUTTON_SEPARATION;   // vertical distance between each row of buttons


    // place category buttons
    int col = CATEGORY_BUTTONS_PER_ROW;
    int row = -1;
    for (auto& entry : m_category_buttons) {
        if (col >= static_cast<int>(CATEGORY_BUTTONS_PER_ROW)) {
            col = 0;
            ++row;
        }
        GG::Pt ul(BUTTON_EDGE_PAD + col*COL_OFFSET, BUTTON_EDGE_PAD + row*ROW_OFFSET);
        GG::Pt lr = ul + GG::Pt(BUTTON_WIDTH, BUTTON_HEIGHT);
        entry.second->SizeMove(ul, lr);
        ++col;
    }
    const auto NUM_CATEGORY_BUTTON_ROWS = row;


    // reset row / column
    col = CATEGORY_BUTTONS_PER_ROW;
    row = 0;

    // place availability buttons
    for (auto& button : m_availabilities_buttons) {
        if (col >= TOTAL_BUTTONS_PER_ROW) {
            col = CATEGORY_BUTTONS_PER_ROW;
            row++;
        }

        auto ul = GG::Pt{BUTTON_EDGE_PAD + col*COL_OFFSET, BUTTON_EDGE_PAD + row*ROW_OFFSET};
        auto lr = ul + GG::Pt{BUTTON_WIDTH, BUTTON_HEIGHT};
        button->SizeMove(ul, lr);
        col++;
    }

    const auto TOTAL_ROWS = std::max(NUM_CATEGORY_BUTTON_ROWS, row);
    // place policies list
    m_policies_list->SizeMove(GG::Pt{GG::X0, BUTTON_EDGE_PAD + ROW_OFFSET*(TOTAL_ROWS + 1)},
                              GG::Pt{ClientWidth(), ClientHeight() - GG::Y{INNER_BORDER_ANGLE_OFFSET}});

    // adjust size of policies
    GG::Pt slot_size = GG::Pt(SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT);
    int text_pts = ClientUI::Pts();

    auto gov_wnd = std::dynamic_pointer_cast<GovernmentWnd>(Parent());
    if (gov_wnd) {
        slot_size = gov_wnd->GetPolicySlotSize();
        text_pts = gov_wnd->GetPolicyTextSize();
    }

    m_policies_list->ResizePolicies(slot_size, text_pts);
}

void GovernmentWnd::PolicyPalette::HandlePolicyClicked(const Policy* policy_type,
                                                       GG::Flags<GG::ModKey> modkeys)
{ PolicyClickedSignal(policy_type, modkeys); }

void GovernmentWnd::PolicyPalette::HandlePolicyRightClicked(const Policy* policy_type,
                                                            const GG::Pt& pt)
{ PolicyRightClickedSignal(policy_type, pt); }

void GovernmentWnd::PolicyPalette::ShowCategory(const std::string& category,
                                                bool refresh_list)
{
    if (!m_category_buttons.count(category)) {
        ErrorLogger() << "PolicyPalette::ShowCategory was passed an invalid category name: " << category;
        return;
    }
    m_policies_list->ShowCategory(category, refresh_list);
    m_category_buttons[category]->SetCheck();
}

void GovernmentWnd::PolicyPalette::ShowAllCategories(bool refresh_list) {
    m_policies_list->ShowAllCategories(refresh_list);
    for (auto& entry : m_category_buttons)
        entry.second->SetCheck();
}

void GovernmentWnd::PolicyPalette::HideCategory(const std::string& category,
                                                bool refresh_list)
{
    if (!m_category_buttons.count(category)) {
        ErrorLogger() << "PolicyPalette::HideCategory was passed an invalid category name: " << category;
        return;
    }
    m_policies_list->HideCategory(category, refresh_list);
    m_category_buttons[category]->SetCheck(false);
}

void GovernmentWnd::PolicyPalette::HideAllCategories(bool refresh_list) {
    m_policies_list->HideAllCategories(refresh_list);
    for (auto& entry : m_category_buttons)
        entry.second->SetCheck(false);
}

void GovernmentWnd::PolicyPalette::ToggleCategory(const std::string& category, bool refresh_list) {
    if (!m_category_buttons.count(category)) {
        ErrorLogger() << "PolicyPalette::ToggleCategory was passed an invalid category name: " << category;
        return;
    }

    const auto& categories_shown = m_policies_list->GetCategoriesShown();
    if (categories_shown.count(category))
        HideCategory(category, refresh_list);
    else
        ShowCategory(category, refresh_list);
}

void GovernmentWnd::PolicyPalette::ToggleAllCategories(bool refresh_list) {
    const auto& categories_shown = m_policies_list->GetCategoriesShown();
    if (categories_shown.size() == m_category_buttons.size())
        HideAllCategories(refresh_list);
    else
        ShowAllCategories(refresh_list);
}

void GovernmentWnd::PolicyPalette::ToggleAvailability(Availability type) {
    auto idx = static_cast<std::underlying_type_t<Availability>>(type);
    if (idx >= m_availabilities_buttons.size())
        return;
    m_availabilities_state.ToggleAvailability(type);
    bool state = m_availabilities_state.GetAvailability(type);

    auto& button = m_availabilities_buttons[idx];
    button->SetCheck(state);
    Populate();
}

void GovernmentWnd::PolicyPalette::Populate() {
    m_policies_list->Populate();
    DoLayout();
}


//////////////////////////////////////////////////
// PolicySlotControl                            //
//////////////////////////////////////////////////
/** UI representation and drop-target for policies of a government.
  * PolicyControl may be dropped into slots to add the corresponding policies to
  * the government, or the policy may be set programmatically with SetPolicy(). */
class PolicySlotControl : public GG::Control {
public:
    PolicySlotControl();
    PolicySlotControl(std::string slot_category, int category_index, unsigned int slot_index);
    void CompleteConstruction() override;

    const std::string&  SlotCategory() const    { return m_slot_category; }
    const Policy*       GetPolicy() const;
    int                 CategoryIndex() const   { return m_category_index; }
    unsigned int        SlotIndex() const       { return m_slot_index; }

    void StartingChildDragDrop(const GG::Wnd* wnd, const GG::Pt& offset) override;
    void CancellingChildDragDrop(const std::vector<const GG::Wnd*>& wnds) override;
    void AcceptDrops(const GG::Pt& pt, std::vector<std::shared_ptr<GG::Wnd>> wnds,
                     GG::Flags<GG::ModKey> mod_keys) override;
    void ChildrenDraggedAway(const std::vector<GG::Wnd*>& wnds,
                             const GG::Wnd* destination) override;
    void DragDropEnter(const GG::Pt& pt, std::map<const Wnd*, bool>& drop_wnds_acceptable,
                       GG::Flags<GG::ModKey> mod_keys) override;
    void DragDropLeave() override;
    void Resize(const GG::Pt& sz, const int pts = ClientUI::Pts());
    void Render() override;

    void Highlight(bool actually = true);

    void SetPolicy(const std::string& policy_name); //!< used to programmatically set the PolicyControl in this slot.
    void SetPolicy(const Policy* policy);           //!< used to programmatically set the PolicyControl in this slot.

    /** emitted when the contents of a slot are altered by the dragging
      * a PolicyControl in or out of the slot.  signal should be caught and the
      * slot contents set using SetPolicy accordingly */
    mutable boost::signals2::signal<void (const Policy*, bool)> SlotContentsAlteredSignal;
    mutable boost::signals2::signal<void (const Policy*, GG::Flags<GG::ModKey>)> PolicyClickedSignal;

protected:
    bool EventFilter(GG::Wnd* w, const GG::WndEvent& event) override;
    void DropsAcceptable(DropsAcceptableIter first, DropsAcceptableIter last,
                         const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) const override;

private:
    bool                                m_highlighted = false;
    std::string                         m_slot_category;
    int                                 m_category_index = -1;
    unsigned int                        m_slot_index = 0;
    std::shared_ptr<PolicyControl>      m_policy_control = nullptr;
    std::shared_ptr<GG::StaticGraphic>  m_background = nullptr;
};

PolicySlotControl::PolicySlotControl() :
    GG::Control(GG::X0, GG::Y0, SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT, GG::INTERACTIVE)
{}

PolicySlotControl::PolicySlotControl(std::string slot_category, int category_index,
                                     unsigned int slot_index) :
    GG::Control(GG::X0, GG::Y0, SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT, GG::INTERACTIVE),
    m_slot_category(std::move(slot_category)),
    m_category_index(category_index),
    m_slot_index(slot_index)
{}

void PolicySlotControl::CompleteConstruction() {
    GG::Control::CompleteConstruction();

    m_background = GG::Wnd::Create<GG::StaticGraphic>(SlotBackgroundTexture(m_slot_category), GG::GRAPHIC_FITGRAPHIC | GG::GRAPHIC_PROPSCALE);
    m_background->Resize(GG::Pt(SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT));
    m_background->Show();
    AttachChild(m_background);

    SetBrowseModeTime(GetOptionsDB().Get<int>("ui.tooltip.delay"));

    SetBrowseInfoWnd(GG::Wnd::Create<IconTextBrowseWnd>(
        SlotBackgroundTexture(m_slot_category),
        UserString(m_slot_category),
        UserString("SL_TOOLTIP_DESC")
    ));
}

bool PolicySlotControl::EventFilter(GG::Wnd* w, const GG::WndEvent& event) {
    if (w == this)
        return false;

    switch (event.Type()) {
    case GG::WndEvent::EventType::DragDropEnter:
    case GG::WndEvent::EventType::DragDropHere:
    case GG::WndEvent::EventType::CheckDrops:
    case GG::WndEvent::EventType::DragDropLeave:
    case GG::WndEvent::EventType::DragDroppedOn:
        HandleEvent(event);
        return true;
        break;
    default:
        return false;
    }
}

void PolicySlotControl::DropsAcceptable(DropsAcceptableIter first, DropsAcceptableIter last,
                                        const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) const
{
    for (DropsAcceptableIter it = first; it != last; ++it)
        it->second = false;

    // if more than one control dropped somehow, reject all
    if (std::distance(first, last) != 1)
        return;

    int empire_id = GGHumanClientApp::GetApp()->EmpireID();
    const Empire* empire = GetEmpire(empire_id);  // may be nullptr
    if (!empire)
        return;

    for (DropsAcceptableIter it = first; it != last; ++it) {
        if (it->first->DragDropDataType() != POLICY_CONTROL_DROP_TYPE_STRING)
            continue;
        const auto policy_control = boost::polymorphic_downcast<const PolicyControl* const>(it->first);
        const Policy* policy = policy_control->GetPolicy();
        if (policy &&
            policy->Category() == m_slot_category &&
            policy_control != m_policy_control.get() &&
            empire->PolicyAvailable(policy->Name()) &&
            empire->PolicyPrereqsAndExclusionsOK(policy->Name()))
        {
            it->second = true;
            return;
        }
    }
}

const Policy* PolicySlotControl::GetPolicy() const {
    if (m_policy_control)
        return m_policy_control->GetPolicy();
    else
        return nullptr;
}

void PolicySlotControl::StartingChildDragDrop(const GG::Wnd* wnd, const GG::Pt& offset) {
    if (!m_policy_control)
        return;

    const auto control = dynamic_cast<const PolicyControl*>(wnd);
    if (!control)
        return;

    if (control == m_policy_control.get())
        m_policy_control->Hide();
}

void PolicySlotControl::CancellingChildDragDrop(const std::vector<const GG::Wnd*>& wnds) {
    if (!m_policy_control)
        return;

    for (const auto& wnd : wnds) {
        const auto control = dynamic_cast<const PolicyControl*>(wnd);
        if (!control)
            continue;

        if (control == m_policy_control.get())
            m_policy_control->Show();
    }
}

void PolicySlotControl::AcceptDrops(const GG::Pt& pt, std::vector<std::shared_ptr<GG::Wnd>> wnds,
                                    GG::Flags<GG::ModKey> mod_keys)
{
    if (wnds.size() != 1)
        ErrorLogger() << "PolicySlotControl::AcceptDrops given multiple wnds unexpectedly...";

    const auto& wnd = *(wnds.begin());
    auto* control = boost::polymorphic_downcast<const PolicyControl*>(wnd.get());
    const Policy* policy_type = control ? control->GetPolicy() : nullptr;

    if (policy_type)
        SlotContentsAlteredSignal(policy_type, (mod_keys & GG::MOD_KEY_CTRL));
}

void PolicySlotControl::ChildrenDraggedAway(const std::vector<GG::Wnd*>& wnds,
                                            const GG::Wnd* destination)
{
    if (wnds.empty())
        return;
    const GG::Wnd* wnd = wnds.front();
    const auto policy_control = dynamic_cast<const PolicyControl*>(wnd);
    if (policy_control != m_policy_control.get())
        return;
    DetachChildAndReset(m_policy_control);
    SlotContentsAlteredSignal(nullptr, false);
}

void PolicySlotControl::DragDropEnter(const GG::Pt& pt,
                                      std::map<const Wnd*, bool>& drop_wnds_acceptable,
                                      GG::Flags<GG::ModKey> mod_keys)
{

    if (drop_wnds_acceptable.empty())
        return;

    DropsAcceptable(drop_wnds_acceptable.begin(), drop_wnds_acceptable.end(), pt, mod_keys);

    // Note:  If this PolicySlotControl is being dragged over this indicates
    //        the dragged policy would replace this policy.
    if (drop_wnds_acceptable.begin()->second && m_policy_control)
        m_policy_control->Hide();
}

void PolicySlotControl::DragDropLeave() {
    // Note:  If m_policy_control is being dragged, this does nothing, because it is detached.
    //        If this PolicySlotControl is being dragged over this indicates the dragged policy would
    //        replace this policy.
    if (m_policy_control && !GG::GUI::GetGUI()->DragDropWnd(m_policy_control.get()))
        m_policy_control->Show();
}

void PolicySlotControl::Resize(const GG::Pt& sz, const int pts) {
    if (m_policy_control)
        m_policy_control->Resize(sz, pts);

    if (m_background)
        m_background->Resize(sz);

    GG::Control::Resize(sz);
}

void PolicySlotControl::Render()
{}

void PolicySlotControl::Highlight(bool actually)
{ m_highlighted = actually; }

void PolicySlotControl::SetPolicy(const std::string& policy_name)
{ SetPolicy( ::GetPolicy(policy_name)); }

void PolicySlotControl::SetPolicy(const Policy* policy) {
    // remove existing policy control, if any
    DetachChildAndReset(m_policy_control);

    if (!policy)
        return;

    // create new policy control for passed in policy_type
    m_policy_control = GG::Wnd::Create<PolicyControl>(policy);
    AttachChild(m_policy_control);
    m_policy_control->InstallEventFilter(shared_from_this());

    // single click shows encyclopedia data
    m_policy_control->ClickedSignal.connect(PolicyClickedSignal);

    // double click clears slot
    m_policy_control->DoubleClickedSignal.connect(
        [this](const Policy*){ this->SlotContentsAlteredSignal(nullptr, false); });
    SetBrowseModeTime(GetOptionsDB().Get<int>("ui.tooltip.delay"));

    m_policy_control->SetBrowseInfoWnd(GG::Wnd::Create<IconTextBrowseWnd>(
        ClientUI::PolicyIcon(policy->Name()),
        UserString(policy->Name()) + " (" + UserString(policy->Category()) + ")",
        UserString(policy->Description())
    ));
}

/** PoliciesListBox accepts policies that are being removed from a PolicySlotControl.*/
void PoliciesListBox::DropsAcceptable(DropsAcceptableIter first, DropsAcceptableIter last,
                                      const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) const
{
    for (DropsAcceptableIter it = first; it != last; ++it)
        it->second = false;

    // if more than one control dropped somehow, reject all
    if (std::distance(first, last) != 1)
        return;

    const auto&& parent = first->first->Parent();
    if (first->first->DragDropDataType() == POLICY_CONTROL_DROP_TYPE_STRING
        && parent
        && dynamic_cast<const PolicySlotControl*>(parent.get()))
    {
        first->second = true;
    }
}


//////////////////////////////////////////////////
// GovernmentWnd::MainPanel                     //
//////////////////////////////////////////////////
class GovernmentWnd::MainPanel : public GG::Wnd {
public:
    MainPanel(GG::X w, GG::Y h);
    void CompleteConstruction() override;

    std::vector<std::string> Policies() const; //!< returns vector of names of policies in slots of current shown design.  empty slots are represented with empty string

    //void LClick(const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) override;
    void AcceptDrops(const GG::Pt& pt, std::vector<std::shared_ptr<GG::Wnd>> wnds, GG::Flags<GG::ModKey> mod_keys) override;
    void SizeMove(const GG::Pt& ul, const GG::Pt& lr) override;
    void Sanitize();
    void Refresh();

    void SetPolicy(const std::string& policy_name, unsigned int slot);  //!< puts specified policy in specified slot.  does nothing if slot is out of range of available slots for category
    void SetPolicy(const Policy* policy, unsigned int slot);            //!< Sets the policy in \p slot to \p policy
    void SetPolicies(const std::vector<std::string>& policies);         //!< puts specified policies in slots.  attempts to put each policy into the slot corresponding to its place in the passed vector.  if a policy cannot be placed, it is ignored.  more policies than there are slots available are ignored, and slots for which there are insufficient policies in the passed vector are unmodified

    /** If a suitable slot is available, adds the specified policy to the
      * government. */
    void AddPolicy(const Policy* policy);
    bool CanPolicyBeAdded(const Policy* policy) const;
    void ClearPolicies();   //!< removes all policies from government
    void ClearPolicy(const std::string& policy_name);

    mutable boost::signals2::signal<void (const Policy*, GG::Flags<GG::ModKey>)> PolicyClickedSignal;

protected:
    void DropsAcceptable(DropsAcceptableIter first, DropsAcceptableIter last,
                         const GG::Pt& pt, GG::Flags<GG::ModKey> mod_keys) const override;

private:
    void Populate();        //!< creates and places SlotControls for empire
    void DoLayout();        //!< positions SlotControls
    bool AddPolicyEmptySlot(const Policy* policy, int slot_number);
    int FindEmptySlotForPolicy(const Policy* policy) const;

    std::vector<std::shared_ptr<PolicySlotControl>> m_slots;
    std::shared_ptr<GG::Button>                     m_clear_button;
};

GovernmentWnd::MainPanel::MainPanel(GG::X w, GG::Y h) :
    GG::Wnd(GG::X0, GG::Y0, w, h, GG::ONTOP | GG::INTERACTIVE)
{}

void GovernmentWnd::MainPanel::CompleteConstruction() {
    SetChildClippingMode(ChildClippingMode::ClipToClient);

    m_clear_button = Wnd::Create<CUIButton>(UserString("GOVERNMENT_WND_CLEAR"));
    AttachChild(m_clear_button);

    m_clear_button->LeftClickedSignal.connect(
        boost::bind(&GovernmentWnd::MainPanel::ClearPolicies, this));

    GG::Wnd::CompleteConstruction();

    DoLayout();
}

std::vector<std::string> GovernmentWnd::MainPanel::Policies() const {
    std::vector<std::string> retval;
    retval.reserve(m_slots.size());
    for (const auto& slot : m_slots) {
        const Policy* policy_type = slot->GetPolicy();
        if (policy_type)
            retval.push_back(policy_type->Name());
        else
            retval.emplace_back();
    }
    return retval;
}

void GovernmentWnd::MainPanel::SizeMove(const GG::Pt& ul, const GG::Pt& lr) {
    const GG::Pt old_size = Size();
    GG::Wnd::SizeMove(ul, lr);
    if (old_size != Size())
        DoLayout();
}

void GovernmentWnd::MainPanel::Sanitize()
{ void ClearPolicies(); }

void GovernmentWnd::MainPanel::Refresh()
{
    Populate();
    DoLayout();  // necessary?
}

void GovernmentWnd::MainPanel::SetPolicy(const std::string& policy_name, unsigned int slot)
{ SetPolicy(GetPolicy(policy_name), slot); }

namespace {
    // returns vector of category names and indices within category
    std::vector<std::pair<int, std::string_view>> ConcatenatedCategorySlots(const Empire* empire) {
        std::vector<std::pair<int, std::string_view>> retval;
        if (!empire)
            return retval;

        // for every slot in every category, add entry to retval in series. count up separately
        // for each slot in each category, to find index for each slot in that cateogry
        for (auto& [cat_name, num_slots_in_cat] : empire->TotalPolicySlots()) {
            for (unsigned int n = 0; n < static_cast<unsigned int>(std::max(0, num_slots_in_cat)); ++n) // treat negative numbers of slots as 0
                retval.emplace_back(n, cat_name);
        }

        return retval;
    }

    std::pair<int, std::string_view> OverallSlotToCategoryAndSlot(const Empire* empire, int overall_slot) {
        if (!empire || overall_slot < 0)
            return {0, ""};

        auto empire_slots = ConcatenatedCategorySlots(empire);
        if (overall_slot >= static_cast<int>(empire_slots.size()))
            return {0, ""};

        return empire_slots[overall_slot];
    }
}

void GovernmentWnd::MainPanel::SetPolicy(const Policy* policy, unsigned int slot) {
    DebugLogger() << "GovernmentWnd::MainPanel::SetPolicy(" << (policy ? policy->Name() : "no policy") << ", slot " << slot << ")";

    if (slot >= m_slots.size()) {
        ErrorLogger() << "GovernmentWnd::MainPanel::SetPolicy specified nonexistant slot";
        return;
    }

    ScriptingContext context;

    int empire_id = GGHumanClientApp::GetApp()->EmpireID();
    auto empire = context.GetEmpire(empire_id);  // may be nullptr
    if (!empire) {
        ErrorLogger() << "GovernmentWnd::MainPanel::SetPolicy has no empire to set policies for";
        return;
    }

    // what category and slot is policy being adopted in
    auto [adopt_in_category_slot, adopt_in_category] = OverallSlotToCategoryAndSlot(empire.get(), slot);
    if (adopt_in_category.empty()) {
        ErrorLogger() << "GovernmentWnd::MainPanel::SetPolicy specified invalid slot: " << slot;
        return;
    }

    // what slots are available...
    auto total_policy_slots = empire->TotalPolicySlots();
    auto total_policy_slots_it = total_policy_slots.find(adopt_in_category);
    if (total_policy_slots_it == total_policy_slots.end()) {
        ErrorLogger() << "GovernmentWnd::MainPanel::SetPolicy asked to adopt in category " << adopt_in_category << " which has no slots";
        return;
    }
    if (total_policy_slots_it->second <= adopt_in_category_slot) {
        ErrorLogger() << "GovernmentWnd::MainPanel::SetPolicy asked to adopt in category " << adopt_in_category_slot
                      << " and slot " << adopt_in_category_slot << " which is not an existing slot (max: " << total_policy_slots_it->second;
        return;
    }


    // what, if anything, is already in that slot?
    // category -> slot in category -> policy in slot
    auto initial_cats_slots_policy_adopted = empire->CategoriesSlotsPoliciesAdopted();
    auto& init_slots_adopted{initial_cats_slots_policy_adopted[adopt_in_category]};
    auto initial_policy_name{init_slots_adopted[adopt_in_category_slot]};

    // check if adopting or revoking a policy. If adopting, then pass along the name of
    // the policy to adopt. If de-adeopting, then pass the name of the policy to de-adopt.
    bool adopt = policy;

    if (!adopt && initial_policy_name.empty()) {
        DebugLogger() << "GovernmentWnd::MainPanel::SetPolicy requested to de-adopt policy in slot " << slot
                      << " but that slot is already empty";
        return;
    }

    std::string order_policy_name{adopt ? policy->Name() : initial_policy_name};
    if (adopt && initial_policy_name == order_policy_name) {
        DebugLogger() << "GovernmentWnd::MainPanel::SetPolicy requested to adopt policy " << order_policy_name
                      << " in slot " << slot << " but that policy is already in that slot";
        return;
    }

    // issue order to adopt or revoke
    auto order = std::make_shared<PolicyOrder>(empire_id, std::move(order_policy_name),
                                               std::string{adopt_in_category},
                                               adopt, adopt_in_category_slot);
    GGHumanClientApp::GetApp()->Orders().IssueOrder(std::move(order), context);

    // update UI after policy changes
    empire->UpdateInfluenceSpending(context);
    Populate();
    DoLayout();
    auto gov_wnd = std::dynamic_pointer_cast<GovernmentWnd>(Parent());
    if (gov_wnd)
        gov_wnd->DoLayout();
    context.ContextUniverse().UpdateMeterEstimates(context);
    SidePanel::Refresh();
    FleetUIManager::GetFleetUIManager().RefreshAll();
}

void GovernmentWnd::MainPanel::SetPolicies(const std::vector<std::string>& policies) {
    ClearPolicies();

    unsigned int num_policies = std::min(policies.size(), m_slots.size());
    for (unsigned int slot = 0; slot < num_policies; ++slot)
        this->SetPolicy(policies[slot], slot);
}

void GovernmentWnd::MainPanel::AddPolicy(const Policy* policy)
{ AddPolicyEmptySlot(policy, FindEmptySlotForPolicy(policy)); }

bool GovernmentWnd::MainPanel::CanPolicyBeAdded(const Policy* policy) const
{ return FindEmptySlotForPolicy(policy) >= 0; }

bool GovernmentWnd::MainPanel::AddPolicyEmptySlot(const Policy* policy, int slot_number) {
    if (!policy || slot_number < 0)
        return false;
    SetPolicy(policy, slot_number);
    return true;
}

int GovernmentWnd::MainPanel::FindEmptySlotForPolicy(const Policy* policy) const {
    if (!policy)
        return -1;
    int empire_id = GGHumanClientApp::GetApp()->EmpireID();
    const Empire* empire = GetEmpire(empire_id);

    // reject unavailable and already-adopted policies
    if (!empire || !empire->PolicyAvailable(policy->Name())
        || empire->PolicyAdopted(policy->Name()))
    { return -1; }

    // scan through slots to find one that can "mount" policy
    for (unsigned int i = 0; i < m_slots.size(); ++i) {
        if (m_slots[i]->GetPolicy())
            continue;   // slot already occupied
        const std::string& slot_category = m_slots[i]->SlotCategory();
        if (policy->Category() != slot_category)
            continue;
        return i;
    }

    return -1;
}

void GovernmentWnd::MainPanel::ClearPolicies() {
    for (unsigned int slot = 0; slot < m_slots.size(); ++slot)
        this->SetPolicy(nullptr, slot);
}

void GovernmentWnd::MainPanel::ClearPolicy(const std::string& policy_name) {
    for (auto& slot : m_slots) {
        const Policy* existing_policy = slot->GetPolicy();
        if (!existing_policy)
            continue;
        if (existing_policy->Name() != policy_name)
            continue;
        slot->SetPolicy(nullptr);
    }
}

void GovernmentWnd::MainPanel::Populate() {
    for (const auto& slot: m_slots)
        DetachChild(slot);
    m_slots.clear();

    // loop over policy slots the empire's government has, add slot controls
    int empire_id = GGHumanClientApp::GetApp()->EmpireID();
    const Empire* empire = GetEmpire(empire_id);  // may be nullptr
    if (!empire)
        return;

    auto all_slot_cats = ConcatenatedCategorySlots(empire);
    auto categories_slots_policies = empire->CategoriesSlotsPoliciesAdopted();

    for (unsigned int n = 0; n < all_slot_cats.size(); ++n) {
        // create slot controls for empire's policy slots
        auto& [category_index, category_name] = all_slot_cats[n];
        auto slot_control = GG::Wnd::Create<PolicySlotControl>(std::string{category_name}, category_index, n);
        m_slots.push_back(slot_control);
        AttachChild(slot_control);

        // assign policy controls to slots that correspond to adopted policies
        if (categories_slots_policies.count(category_name)) {
            const auto& slots = categories_slots_policies[category_name];
            if (slots.count(category_index))
                slot_control->SetPolicy(std::string{slots.at(category_index)}); // TODO: avoid string construction if possible...
        }

        // signals to respond to UI manipulation
        slot_control->SlotContentsAlteredSignal.connect(
            boost::bind(static_cast<void (GovernmentWnd::MainPanel::*)(
                const Policy*, unsigned int)>(&GovernmentWnd::MainPanel::SetPolicy),
                                              this, _1, n));
        slot_control->PolicyClickedSignal.connect(PolicyClickedSignal);
    }
}

void GovernmentWnd::MainPanel::DoLayout() {
    const int PTS = ClientUI::Pts();
    const GG::X PTS_WIDE(PTS / 2);           // guess at how wide per character the font needs
    const GG::Y BUTTON_HEIGHT(PTS * 2);
    constexpr int PAD = 6;
    constexpr int GUESSTIMATE_NUM_CHARS_IN_BUTTON_TEXT = 25;    // rough guesstimate... avoid overly long policy class names
    const GG::X BUTTON_WIDTH = PTS_WIDE*GUESSTIMATE_NUM_CHARS_IN_BUTTON_TEXT;

    const GG::Pt lr = ClientSize() + GG::Pt(-GG::X(PAD), -GG::Y(PAD));
    const GG::Pt button_ul = GG::Pt(lr.x - BUTTON_WIDTH, GG::Y0);
    const GG::Pt button_lr = GG::Pt(lr.x, BUTTON_HEIGHT);
    m_clear_button->SizeMove(button_ul, button_lr);

    if (m_slots.empty())
        return;

    // arrange policy slots, start new row when slots overlap with right wnd border
    auto gov_wnd = std::dynamic_pointer_cast<GovernmentWnd>(Parent());
    GG::Pt slot_size = GG::Pt(SLOT_CONTROL_WIDTH, SLOT_CONTROL_HEIGHT);
    int text_pts = ClientUI::Pts();
    if (gov_wnd) {
        slot_size = gov_wnd->GetPolicySlotSize();
        text_pts = gov_wnd->GetPolicyTextSize();
    }

    const GG::X initial_slot_l = GG::X(PAD*2);
    GG::Pt ul = GG::Pt(initial_slot_l, button_lr.y + PAD);

    int count = 0;
    bool first_iteration = true;
    for (auto& slot : m_slots) {

        slot->Resize(GG::Pt(slot_size.x, slot_size.y), text_pts);

        // start of new row
        if (count == 0) {
            ul.x = initial_slot_l;

            if (first_iteration)
                first_iteration = false;
            else
                ul.y += slot->Height() * (1 + POLICY_PAD);
        // no new row, progress in row
        }
        else {
            ul.x += slot->Width() * (1 + POLICY_PAD);
        }

        slot->MoveTo(ul);
        count++;

        // reset count when hitting right border
        if ((count + 1) * slot->Size().x * (1 + POLICY_PAD) > (lr.x - PAD)) {
            count = 0;
        }
    } 
}

void GovernmentWnd::MainPanel::DropsAcceptable(DropsAcceptableIter first,
                                               DropsAcceptableIter last,
                                               const GG::Pt& pt,
                                               GG::Flags<GG::ModKey> mod_keys) const
{
    for (DropsAcceptableIter it = first; it != last; ++it)
        it->second = false;

    // if multiple things dropped simultaneously somehow, reject all
    if (std::distance(first, last) != 1)
        return;
}

void GovernmentWnd::MainPanel::AcceptDrops(const GG::Pt& pt,
                                           std::vector<std::shared_ptr<GG::Wnd>> wnds,
                                           GG::Flags<GG::ModKey> mod_keys)
{}


//////////////////////////////////////////////////
// GovernmentWnd                                //
//////////////////////////////////////////////////
GovernmentWnd::GovernmentWnd(std::string_view config_name) :
    CUIWnd(UserString("MAP_BTN_GOVERNMENT"),
           GG::ONTOP | GG::INTERACTIVE | GG::DRAGABLE | GG::RESIZABLE | CLOSABLE | PINABLE,
           config_name, false)
{}

void GovernmentWnd::CompleteConstruction() {
    Sound::TempUISoundDisabler sound_disabler;
    SetChildClippingMode(ChildClippingMode::ClipToClient);

    m_main_panel = GG::Wnd::Create<MainPanel>(GG::X(100), GG::Y(100));
    AttachChild(m_main_panel);
    m_main_panel->Sanitize();

    m_policy_palette = GG::Wnd::Create<PolicyPalette>(GG::X(100), GG::Y(100));
    AttachChild(m_policy_palette);
    m_policy_palette->PolicyDoubleClickedSignal.connect(
        boost::bind(&GovernmentWnd::MainPanel::AddPolicy, m_main_panel, _1));
    m_policy_palette->ClearPolicySignal.connect(
        boost::bind(&GovernmentWnd::MainPanel::ClearPolicy, m_main_panel, _1));

    m_policy_size_buttons = GG::Wnd::Create<GG::RadioButtonGroup>(GG::Orientation::HORIZONTAL);
    m_policy_size_buttons->ExpandButtons(true);
    m_policy_size_buttons->ExpandButtonsProportionally(true);

    auto large_size_icon = std::make_shared<GG::SubTexture>(ClientUI::GetTexture(ClientUI::ArtDir() / "icons" / "buttons" / "policy_large.png"));
    auto medium_size_icon = std::make_shared<GG::SubTexture>(ClientUI::GetTexture(ClientUI::ArtDir() / "icons" / "buttons" / "policy_medium.png"));
    auto small_size_icon = std::make_shared<GG::SubTexture>(ClientUI::GetTexture(ClientUI::ArtDir() / "icons" / "buttons" / "policy_small.png"));

    m_policy_size_buttons->AddButton(
        GG::Wnd::Create<CUIStateButton>("", GG::FORMAT_LEFT, std::make_shared<CUIIconButtonRepresenter>(large_size_icon, GG::CLR_WHITE)));
    m_policy_size_buttons->AddButton(
        GG::Wnd::Create<CUIStateButton>("", GG::FORMAT_LEFT, std::make_shared<CUIIconButtonRepresenter>(medium_size_icon, GG::CLR_WHITE)));
    m_policy_size_buttons->AddButton(
        GG::Wnd::Create<CUIStateButton>("", GG::FORMAT_LEFT, std::make_shared<CUIIconButtonRepresenter>(small_size_icon, GG::CLR_WHITE)));
    AttachChild(m_policy_size_buttons);
    m_policy_size_buttons->SetCheck(0);

    auto zoom_to_policy_action = [](const Policy* policy, GG::Flags<GG::ModKey> modkeys) { ClientUI::GetClientUI()->ZoomToPolicy(policy->Name()); };
    //m_main_panel->PolicyClickedSignal.connect(zoom_to_policy_action);
    m_policy_palette->PolicyClickedSignal.connect(zoom_to_policy_action);
    m_policy_size_buttons->ButtonChangedSignal.connect(boost::bind(&GovernmentWnd::PolicySizeButtonClicked, this, _1));

    CUIWnd::CompleteConstruction();
    DoLayout();
}

void GovernmentWnd::SizeMove(const GG::Pt& ul, const GG::Pt& lr) {
    const GG::Pt old_size = Size();
    CUIWnd::SizeMove(ul, lr);
    if (old_size != Size())
        DoLayout();
}

void GovernmentWnd::Reset()
{ Refresh(); }

void GovernmentWnd::Sanitize()
{ m_main_panel->Sanitize(); }

void GovernmentWnd::Refresh() {
    m_policy_palette->Populate();
    m_main_panel->Refresh();
}

double GovernmentWnd::GetPolicyZoomFactor() {
    switch (m_policy_size_buttons->CheckedButton()) {
    case 0: return 1;
    case 1: return 0.75;
    case 2: return 0.5;
    }

    DebugLogger() << "GovernmentWnd::GetPolicyZoomFactor(): m_policy_buttons no button checked.";
    return 1;
}

GG::Pt GovernmentWnd::GetPolicySlotSize() {
    double zoom_factor = GetPolicyZoomFactor();
    const GG::X slot_width = GG::X(SLOT_CONTROL_WIDTH * zoom_factor);
    const GG::Y slot_height = GG::Y(SLOT_CONTROL_HEIGHT * zoom_factor);
    return GG::Pt(slot_width, slot_height);
}

int GovernmentWnd::GetPolicyTextSize() {
    double zoom_factor = GetPolicyZoomFactor();
    return static_cast<int>(ClientUI::Pts() * zoom_factor);
}

void GovernmentWnd::DoLayout() {
    const GG::Y BUTTON_HEIGHT(ClientUI::Pts() * 2);
    constexpr GG::Pt palette_ul(GG::X0, GG::Y0);
    const GG::Pt palette_lr(palette_ul + GG::Pt(ClientWidth(), ClientHeight() / 2));
    const int num_size_buttons = static_cast<int>(m_policy_size_buttons->NumButtons());

    const GG::Pt main_ul(palette_ul + GG::Pt(GG::X0, ClientHeight() / 2 + PAD));
    const GG::Pt main_lr(ClientWidth(), ClientHeight() - GG::Y(INNER_BORDER_ANGLE_OFFSET));

    m_main_panel->SizeMove(main_ul, main_lr);
    m_policy_palette->SizeMove(palette_ul, palette_lr);

    const GG::Pt size_buttons_ul = main_ul + GG::Pt(GG::X(PAD), GG::Y0);
    const GG::Pt size_buttons_lr = main_ul + GG::Pt((GG::X(PAD) + POLICY_SIZE_BUTTON_WIDTH) * num_size_buttons, BUTTON_HEIGHT);

    m_policy_size_buttons->SizeMove(size_buttons_ul, size_buttons_lr);
}

void GovernmentWnd::PolicySizeButtonClicked(std::size_t idx) {
    Refresh();
}

void GovernmentWnd::EnableOrderIssuing(bool enable)
{}

void GovernmentWnd::CloseClicked()
{ ClosingSignal(); }

