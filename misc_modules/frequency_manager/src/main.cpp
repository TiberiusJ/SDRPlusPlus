#include <imgui.h>
#include <utils/flog.h>
#include <module.h>
#include <gui/gui.h>
#include <gui/style.h>
#include <core.h>
#include <thread>
#include <mutex>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <radio_interface.h>
#include <signal_path/signal_path.h>
#include <vector>
#include <gui/tuner.h>
#include <gui/file_dialogs.h>
#include <utils/freq_formatting.h>
#include <gui/dialogs/dialog_box.h>
#include <fstream>

SDRPP_MOD_INFO{
    /* Name:            */ "frequency_manager",
    /* Description:     */ "Frequency manager module with bookmark scanner for SDR++",
    /* Author:          */ "Ryzerth;Zimm",
    /* Version:         */ 0, 3, 1,
    /* Max instances    */ 1
};

struct FrequencyBookmark {
    double frequency;
    double bandwidth;
    int mode;
    bool selected;
};

struct WaterfallBookmark {
    std::string listName;
    std::string bookmarkName;
    FrequencyBookmark bookmark;
};

ConfigManager config;

const char* demodModeList[] = {
    "NFM",
    "WFM",
    "AM",
    "DSB",
    "USB",
    "CW",
    "LSB",
    "RAW"
};

const char* demodModeListTxt = "NFM\0WFM\0AM\0DSB\0USB\0CW\0LSB\0RAW\0";

enum {
    BOOKMARK_DISP_MODE_OFF,
    BOOKMARK_DISP_MODE_TOP,
    BOOKMARK_DISP_MODE_BOTTOM,
    _BOOKMARK_DISP_MODE_COUNT
};

const char* bookmarkDisplayModesTxt = "Off\0Top\0Bottom\0";

class FrequencyManagerModule : public ModuleManager::Instance {
public:
    FrequencyManagerModule(std::string name) {
        this->name = name;

        config.acquire();
        std::string selList = config.conf["selectedList"];
        bookmarkDisplayMode = config.conf["bookmarkDisplayMode"];
        config.release();

        refreshLists();
        loadByName(selList);
        refreshWaterfallBookmarks();

        fftRedrawHandler.ctx = this;
        fftRedrawHandler.handler = fftRedraw;
        inputHandler.ctx = this;
        inputHandler.handler = fftInput;

        gui::menu.registerEntry(name, menuHandler, this, NULL);
        gui::waterfall.onFFTRedraw.bindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.bindHandler(&inputHandler);
    }

    ~FrequencyManagerModule() {
        stopScan();
        gui::menu.removeEntry(name);
        gui::waterfall.onFFTRedraw.unbindHandler(&fftRedrawHandler);
        gui::waterfall.onInputProcess.unbindHandler(&inputHandler);
    }

    void postInit() {}

    void enable() {
        enabled = true;
    }

    void disable() {
        stopScan();
        enabled = false;
    }

    bool isEnabled() {
        return enabled;
    }

private:
    static void applyBookmark(FrequencyBookmark bm, std::string vfoName) {
        if (vfoName == "") {
            gui::waterfall.setCenterFrequency(bm.frequency);
            gui::waterfall.centerFreqMoved = true;
        }
        else {
            if (core::modComManager.interfaceExists(vfoName)) {
                if (core::modComManager.getModuleName(vfoName) == "radio") {
                    int mode = bm.mode;
                    float bandwidth = bm.bandwidth;
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_MODE, &mode, NULL);
                    core::modComManager.callInterface(vfoName, RADIO_IFACE_CMD_SET_BANDWIDTH, &bandwidth, NULL);
                }
            }
            tuner::tune(tuner::TUNER_MODE_NORMAL, vfoName, bm.frequency);
        }
    }

    float getMaxLevel(float* data, double freq, double width, int dataWidth, double wfStart, double wfWidth) {
        double low = freq - (width / 2.0);
        double high = freq + (width / 2.0);
        int lowId = std::clamp<int>((low - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        int highId = std::clamp<int>((high - wfStart) * (double)dataWidth / wfWidth, 0, dataWidth - 1);
        float maxVal = -INFINITY;
        for (int i = lowId; i <= highId; i++) {
            if (data[i] > maxVal) { maxVal = data[i]; }
        }
        return maxVal;
    }

    void startScan() {
        if (scanning || bookmarks.empty()) { return; }
        scanIndex = 0;
        scanReceiving = false;
        scanTuning = true;
        skipRequested = false;
        scanning = true;
        scanThread = std::thread(&FrequencyManagerModule::scanWorker, this);
    }

    void stopScan() {
        if (!scanning) { return; }
        scanning = false;
        if (scanThread.joinable()) {
            scanThread.join();
        }
    }

    void scanWorker() {
        bool firstHop = true;
        while (scanning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            std::lock_guard<std::mutex> lck(scanMtx);

            if (bookmarks.empty() || gui::waterfall.selectedVFO.empty()) {
                scanning = false;
                break;
            }

            if (scanIndex >= (int)bookmarks.size()) {
                scanIndex = 0;
            }

            auto it = bookmarks.begin();
            std::advance(it, scanIndex);
            std::string currentBmName = it->first;
            FrequencyBookmark currentBm = it->second;
            auto now = std::chrono::high_resolution_clock::now();

            if (firstHop) {
                firstHop = false;
                activeScanName = currentBmName;
                applyBookmark(currentBm, gui::waterfall.selectedVFO);
                lastTuneTime = now;
                scanTuning = true;
                scanReceiving = false;
                continue;
            }

            if (skipRequested) {
                skipRequested = false;
                scanReceiving = false;
                scanIndex = (scanIndex + 1) % (int)bookmarks.size();
                auto nextIt = bookmarks.begin();
                std::advance(nextIt, scanIndex);
                activeScanName = nextIt->first;
                applyBookmark(nextIt->second, gui::waterfall.selectedVFO);
                lastTuneTime = now;
                scanTuning = true;
                continue;
            }

            if (scanTuning) {
                if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastTuneTime).count() >= tuningTimeMs) {
                    scanTuning = false;
                }
                continue;
            }

            int dataWidth = 0;
            float* data = gui::waterfall.acquireLatestFFT(dataWidth);
            if (!data) { continue; }

            double wfCenter = gui::waterfall.getViewOffset() + gui::waterfall.getCenterFrequency();
            double wfWidth = gui::waterfall.getViewBandwidth();
            double wfStart = wfCenter - (wfWidth / 2.0);
            double vfoWidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
            if (vfoWidth <= 0) { vfoWidth = currentBm.bandwidth > 0 ? currentBm.bandwidth : 12500.0; }

            float maxLevel = getMaxLevel(data, currentBm.frequency, vfoWidth * 0.5, dataWidth, wfStart, wfWidth);
            gui::waterfall.releaseLatestFFT();

            if (scanReceiving) {
                if (maxLevel >= scanLevel) {
                    lastSignalTime = now;
                }
                else if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastSignalTime).count() >= lingerTimeMs) {
                    scanReceiving = false;
                    scanIndex = (scanIndex + 1) % (int)bookmarks.size();
                    auto nextIt = bookmarks.begin();
                    std::advance(nextIt, scanIndex);
                    activeScanName = nextIt->first;
                    applyBookmark(nextIt->second, gui::waterfall.selectedVFO);
                    lastTuneTime = now;
                    scanTuning = true;
                }
            }
            else {
                if (maxLevel >= scanLevel) {
                    scanReceiving = true;
                    lastSignalTime = now;
                }
                else {
                    scanIndex = (scanIndex + 1) % (int)bookmarks.size();
                    auto nextIt = bookmarks.begin();
                    std::advance(nextIt, scanIndex);
                    activeScanName = nextIt->first;
                    applyBookmark(nextIt->second, gui::waterfall.selectedVFO);
                    lastTuneTime = now;
                    scanTuning = true;
                }
            }
        }
    }

    bool bookmarkEditDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        std::string id = "Edit##freq_manager_edit_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedBookmarkName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::BeginTable(("freq_manager_edit_table" + name).c_str(), 2);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Name");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, 1023)) {
                editedBookmarkName = nameBuf;
            }

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Frequency");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            ImGui::InputDouble(("##freq_manager_edit_freq" + name).c_str(), &editedBookmark.frequency);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Bandwidth");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            ImGui::InputDouble(("##freq_manager_edit_bw" + name).c_str(), &editedBookmark.bandwidth);

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::LeftLabel("Mode");
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(200);
            ImGui::Combo(("##freq_manager_edit_mode" + name).c_str(), &editedBookmark.mode, demodModeListTxt);

            ImGui::EndTable();

            bool applyDisabled = (strlen(nameBuf) == 0) || (bookmarks.find(editedBookmarkName) != bookmarks.end() && editedBookmarkName != firstEditedBookmarkName);
            if (applyDisabled) { style::beginDisabled(); }
            if (ImGui::Button("Apply")) {
                open = false;
                if (editOpen) {
                    bookmarks.erase(firstEditedBookmarkName);
                }
                bookmarks[editedBookmarkName] = editedBookmark;
                saveByName(selectedListName);
            }
            if (applyDisabled) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool newListDialog() {
        bool open = true;
        gui::mainWindow.lockWaterfallControls = true;

        float menuWidth = ImGui::GetContentRegionAvail().x;
        std::string id = "New##freq_manager_new_popup_" + name;
        ImGui::OpenPopup(id.c_str());

        char nameBuf[1024];
        strcpy(nameBuf, editedListName.c_str());

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            ImGui::LeftLabel("Name");
            ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
            if (ImGui::InputText(("##freq_manager_edit_name" + name).c_str(), nameBuf, 1023)) {
                editedListName = nameBuf;
            }

            bool alreadyExists = (std::find(listNames.begin(), listNames.end(), editedListName) != listNames.end());

            if (strlen(nameBuf) == 0 || alreadyExists) { style::beginDisabled(); }
            if (ImGui::Button("Apply")) {
                open = false;
                config.acquire();
                if (renameListOpen) {
                    config.conf["lists"][editedListName] = config.conf["lists"][firstEditedListName];
                    config.conf["lists"].erase(firstEditedListName);
                }
                else {
                    config.conf["lists"][editedListName]["showOnWaterfall"] = true;
                    config.conf["lists"][editedListName]["bookmarks"] = json::object();
                }
                refreshWaterfallBookmarks(false);
                config.release(true);
                refreshLists();
                loadByName(editedListName);
            }
            if (strlen(nameBuf) == 0 || alreadyExists) { style::endDisabled(); }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    bool selectListsDialog() {
        gui::mainWindow.lockWaterfallControls = true;
        std::string id = "Select lists##freq_manager_sel_popup_" + name;
        ImGui::OpenPopup(id.c_str());
        bool open = true;

        if (ImGui::BeginPopup(id.c_str(), ImGuiWindowFlags_NoResize)) {
            for (auto [listName, list] : config.conf["lists"].items()) {
                bool shown = list["showOnWaterfall"];
                if (ImGui::Checkbox((listName + "##freq_manager_sel_list_").c_str(), &shown)) {
                    config.acquire();
                    config.conf["lists"][listName]["showOnWaterfall"] = shown;
                    refreshWaterfallBookmarks(false);
                    config.release(true);
                }
            }
            if (ImGui::Button("Ok")) {
                open = false;
            }
            ImGui::EndPopup();
        }
        return open;
    }

    void refreshLists() {
        listNames.clear();
        listNamesTxt = "";

        config.acquire();
        for (auto [_name, list] : config.conf["lists"].items()) {
            listNames.push_back(_name);
            listNamesTxt += _name;
            listNamesTxt += '\0';
        }
        config.release();
    }

    void refreshWaterfallBookmarks(bool lockConfig = true) {
        if (lockConfig) { config.acquire(); }
        waterfallBookmarks.clear();
        for (auto [listName, list] : config.conf["lists"].items()) {
            if (!((bool)list["showOnWaterfall"])) { continue; }
            WaterfallBookmark wbm;
            wbm.listName = listName;
            for (auto [bookmarkName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
                wbm.bookmarkName = bookmarkName;
                wbm.bookmark.frequency = config.conf["lists"][listName]["bookmarks"][bookmarkName]["frequency"];
                wbm.bookmark.bandwidth = config.conf["lists"][listName]["bookmarks"][bookmarkName]["bandwidth"];
                wbm.bookmark.mode = config.conf["lists"][listName]["bookmarks"][bookmarkName]["mode"];
                wbm.bookmark.selected = false;
                waterfallBookmarks.push_back(wbm);
            }
        }
        if (lockConfig) { config.release(); }
    }

    void loadFirst() {
        if (listNames.size() > 0) {
            loadByName(listNames[0]);
            return;
        }
        selectedListName = "";
        selectedListId = 0;
    }

    void loadByName(std::string listName) {
        stopScan();
        std::lock_guard<std::mutex> lck(scanMtx);
        bookmarks.clear();
        if (std::find(listNames.begin(), listNames.end(), listName) == listNames.end()) {
            selectedListName = "";
            selectedListId = 0;
            loadFirst();
            return;
        }
        selectedListId = std::distance(listNames.begin(), std::find(listNames.begin(), listNames.end(), listName));
        selectedListName = listName;
        config.acquire();
        for (auto [bmName, bm] : config.conf["lists"][listName]["bookmarks"].items()) {
            FrequencyBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            fbm.selected = false;
            bookmarks[bmName] = fbm;
        }
        config.release();
    }

    void saveByName(std::string listName) {
        std::lock_guard<std::mutex> lck(scanMtx);
        config.acquire();
        config.conf["lists"][listName]["bookmarks"] = json::object();
        for (auto [bmName, bm] : bookmarks) {
            config.conf["lists"][listName]["bookmarks"][bmName]["frequency"] = bm.frequency;
            config.conf["lists"][listName]["bookmarks"][bmName]["bandwidth"] = bm.bandwidth;
            config.conf["lists"][listName]["bookmarks"][bmName]["mode"] = bm.mode;
        }
        refreshWaterfallBookmarks(false);
        config.release(true);
    }

    static void menuHandler(void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        float menuWidth = ImGui::GetContentRegionAvail().x;

        std::vector<std::string> selectedNames;
        for (auto& [name, bm] : _this->bookmarks) {
            if (bm.selected) { selectedNames.push_back(name); }
        }

        float lineHeight = ImGui::GetTextLineHeightWithSpacing();
        float btnSize = ImGui::CalcTextSize("Rename").x + 8;
        ImGui::SetNextItemWidth(menuWidth - 24 - (2 * lineHeight) - btnSize);
        if (ImGui::Combo(("##freq_manager_list_sel" + _this->name).c_str(), &_this->selectedListId, _this->listNamesTxt.c_str())) {
            _this->loadByName(_this->listNames[_this->selectedListId]);
            config.acquire();
            config.conf["selectedList"] = _this->selectedListName;
            config.release(true);
        }
        ImGui::SameLine();
        if (_this->listNames.size() == 0) { style::beginDisabled(); }
        if (ImGui::Button(("Rename##_freq_mgr_ren_lst_" + _this->name).c_str(), ImVec2(btnSize, 0))) {
            _this->firstEditedListName = _this->listNames[_this->selectedListId];
            _this->editedListName = _this->firstEditedListName;
            _this->renameListOpen = true;
        }
        if (_this->listNames.size() == 0) { style::endDisabled(); }
        ImGui::SameLine();
        if (ImGui::Button(("+##_freq_mgr_add_lst_" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            if (std::find(_this->listNames.begin(), _this->listNames.end(), "New List") == _this->listNames.end()) {
                _this->editedListName = "New List";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "New List (%d)", i);
                    if (std::find(_this->listNames.begin(), _this->listNames.end(), buf) == _this->listNames.end()) { break; }
                }
                _this->editedListName = buf;
            }
            _this->newListOpen = true;
        }
        ImGui::SameLine();
        if (_this->selectedListName == "") { style::beginDisabled(); }
        if (ImGui::Button(("-##_freq_mgr_del_lst_" + _this->name).c_str(), ImVec2(lineHeight, 0))) {
            _this->deleteListOpen = true;
        }
        if (_this->selectedListName == "") { style::endDisabled(); }

        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteListOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::Text("Deleting list named \"%s\". Are you sure?", _this->selectedListName.c_str());
            }) == GENERIC_DIALOG_BUTTON_YES) {
            _this->stopScan();
            config.acquire();
            config.conf["lists"].erase(_this->selectedListName);
            _this->refreshWaterfallBookmarks(false);
            config.release(true);
            _this->refreshLists();
            _this->selectedListId = std::clamp<int>(_this->selectedListId, 0, _this->listNames.size());
            if (_this->listNames.size() > 0) {
                _this->loadByName(_this->listNames[_this->selectedListId]);
            }
            else {
                _this->selectedListName = "";
            }
        }

        if (_this->selectedListName == "") { style::beginDisabled(); }
        ImGui::BeginTable(("freq_manager_btn_table" + _this->name).c_str(), 3);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Add##_freq_mgr_add_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            if (gui::waterfall.selectedVFO == "") {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency();
                _this->editedBookmark.bandwidth = 0;
                _this->editedBookmark.mode = 7;
            }
            else {
                _this->editedBookmark.frequency = gui::waterfall.getCenterFrequency() + sigpath::vfoManager.getOffset(gui::waterfall.selectedVFO);
                _this->editedBookmark.bandwidth = sigpath::vfoManager.getBandwidth(gui::waterfall.selectedVFO);
                _this->editedBookmark.mode = 7;
                if (core::modComManager.getModuleName(gui::waterfall.selectedVFO) == "radio") {
                    int mode;
                    core::modComManager.callInterface(gui::waterfall.selectedVFO, RADIO_IFACE_CMD_GET_MODE, NULL, &mode);
                    _this->editedBookmark.mode = mode;
                }
            }
            _this->editedBookmark.selected = false;
            _this->createOpen = true;

            if (_this->bookmarks.find("New Bookmark") == _this->bookmarks.end()) {
                _this->editedBookmarkName = "New Bookmark";
            }
            else {
                char buf[64];
                for (int i = 1; i < 1000; i++) {
                    sprintf(buf, "New Bookmark (%d)", i);
                    if (_this->bookmarks.find(buf) == _this->bookmarks.end()) { break; }
                }
                _this->editedBookmarkName = buf;
            }
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Remove##_freq_mgr_rem_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->deleteBookmarksOpen = true;
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::TableSetColumnIndex(2);
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Edit##_freq_mgr_edt_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
            _this->editOpen = true;
            _this->editedBookmark = _this->bookmarks[selectedNames[0]];
            _this->editedBookmarkName = selectedNames[0];
            _this->firstEditedBookmarkName = selectedNames[0];
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::EndTable();

        if (ImGui::GenericDialog(("freq_manager_del_list_confirm" + _this->name).c_str(), _this->deleteBookmarksOpen, GENERIC_DIALOG_BUTTONS_YES_NO, [_this]() {
                ImGui::TextUnformatted("Deleting selected bookmaks. Are you sure?");
            }) == GENERIC_DIALOG_BUTTON_YES) {
            _this->stopScan();
            for (auto& _name : selectedNames) { _this->bookmarks.erase(_name); }
            _this->saveByName(_this->selectedListName);
        }

        if (ImGui::BeginTable(("freq_manager_bkm_table" + _this->name).c_str(), 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200.0f * style::uiScale))) {
            ImGui::TableSetupColumn("Name");
            ImGui::TableSetupColumn("Bookmark");
            ImGui::TableSetupScrollFreeze(2, 1);
            ImGui::TableHeadersRow();
            for (auto& [name, bm] : _this->bookmarks) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                bool isHighlighted = _this->scanning ? (_this->activeScanName == name) : bm.selected;
                if (ImGui::Selectable((name + "##_freq_mgr_bkm_name_" + _this->name).c_str(), isHighlighted, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_SelectOnClick)) {
                    if (!_this->scanning) {
                        bm.selected = !bm.selected;
                        if (!ImGui::GetIO().KeyShift && !ImGui::GetIO().KeyCtrl) {
                            for (auto& [_name, _bm] : _this->bookmarks) {
                                if (name == _name) { continue; }
                                _bm.selected = false;
                            }
                        }
                    }
                }
                if (ImGui::TableGetHoveredColumn() >= 0 && ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    _this->stopScan();
                    applyBookmark(bm, gui::waterfall.selectedVFO);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::Text("%s %s", utils::formatFreq(bm.frequency).c_str(), demodModeList[bm.mode]);
            }
            ImGui::EndTable();
        }

        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Apply##_freq_mgr_apply_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->stopScan();
            FrequencyBookmark& bm = _this->bookmarks[selectedNames[0]];
            applyBookmark(bm, gui::waterfall.selectedVFO);
            bm.selected = false;
        }
        if (selectedNames.size() != 1 && _this->selectedListName != "") { style::endDisabled(); }

        // --- Bookmark Scanner Controls ---
        ImGui::Separator();
        ImGui::LeftLabel("Scan Level (dB)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        ImGui::SliderFloat(("##_freq_mgr_scan_lvl_" + _this->name).c_str(), &_this->scanLevel, -120.0f, 0.0f);

        ImGui::LeftLabel("Hop Delay (ms)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##_freq_mgr_scan_tune_" + _this->name).c_str(), &_this->tuningTimeMs, 50, 250)) {
            _this->tuningTimeMs = std::clamp<int>(_this->tuningTimeMs, 100, 5000);
        }

        ImGui::LeftLabel("Hold Time (ms)");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::InputInt(("##_freq_mgr_scan_linger_" + _this->name).c_str(), &_this->lingerTimeMs, 100, 500)) {
            _this->lingerTimeMs = std::clamp<int>(_this->lingerTimeMs, 100, 10000);
        }

        if (_this->bookmarks.empty() && _this->selectedListName != "") { style::beginDisabled(); }
        if (!_this->scanning) {
            if (ImGui::Button(("Start Scan##_freq_mgr_scan_start_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
                _this->startScan();
            }
        }
        else {
            ImGui::BeginTable(("freq_manager_scan_btn_table" + _this->name).c_str(), 2);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (ImGui::Button(("Stop Scan##_freq_mgr_scan_stop_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                _this->stopScan();
            }
            ImGui::TableSetColumnIndex(1);
            if (ImGui::Button(("Skip >>##_freq_mgr_scan_skip_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0))) {
                std::lock_guard<std::mutex> lck(_this->scanMtx);
                _this->skipRequested = true;
            }
            ImGui::EndTable();
        }
        if (_this->bookmarks.empty() && _this->selectedListName != "") { style::endDisabled(); }

        if (_this->scanning) {
            if (_this->scanReceiving) {
                ImGui::TextColored(ImVec4(0, 1, 0, 1), "Active: %s", _this->activeScanName.c_str());
            }
            else {
                ImGui::TextColored(ImVec4(1, 1, 0, 1), "Scanning: %s", _this->activeScanName.c_str());
            }
        }
        ImGui::Separator();

        // Draw import and export buttons
        ImGui::BeginTable(("freq_manager_bottom_btn_table" + _this->name).c_str(), 2);
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (ImGui::Button(("Import##_freq_mgr_imp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->importOpen) {
            _this->importOpen = true;
            _this->importDialog = new pfd::open_file("Import bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" }, pfd::opt::multiselect);
        }

        ImGui::TableSetColumnIndex(1);
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::beginDisabled(); }
        if (ImGui::Button(("Export##_freq_mgr_exp_" + _this->name).c_str(), ImVec2(ImGui::GetContentRegionAvail().x, 0)) && !_this->exportOpen) {
            _this->exportedBookmarks = json::object();
            config.acquire();
            for (auto& _name : selectedNames) {
                _this->exportedBookmarks["bookmarks"][_name] = config.conf["lists"][_this->selectedListName]["bookmarks"][_name];
            }
            config.release();
            _this->exportOpen = true;
            _this->exportDialog = new pfd::save_file("Export bookmarks", "", { "JSON Files (*.json)", "*.json", "All Files", "*" });
        }
        if (selectedNames.size() == 0 && _this->selectedListName != "") { style::endDisabled(); }
        ImGui::EndTable();

        if (ImGui::Button(("Select displayed lists##_freq_mgr_sel_disp_" + _this->name).c_str(), ImVec2(menuWidth, 0))) {
            _this->selectListsOpen = true;
        }

        ImGui::LeftLabel("Bookmark display mode");
        ImGui::SetNextItemWidth(menuWidth - ImGui::GetCursorPosX());
        if (ImGui::Combo(("##_freq_mgr_dms_" + _this->name).c_str(), &_this->bookmarkDisplayMode, bookmarkDisplayModesTxt)) {
            config.acquire();
            config.conf["bookmarkDisplayMode"] = _this->bookmarkDisplayMode;
            config.release(true);
        }

        if (_this->selectedListName == "") { style::endDisabled(); }

        if (_this->createOpen) { _this->createOpen = _this->bookmarkEditDialog(); }
        if (_this->editOpen) { _this->editOpen = _this->bookmarkEditDialog(); }
        if (_this->newListOpen) { _this->newListOpen = _this->newListDialog(); }
        if (_this->renameListOpen) { _this->renameListOpen = _this->newListDialog(); }
        if (_this->selectListsOpen) { _this->selectListsOpen = _this->selectListsDialog(); }

        if (_this->importOpen && _this->importDialog->ready()) {
            _this->importOpen = false;
            std::vector<std::string> paths = _this->importDialog->result();
            if (paths.size() > 0 && _this->listNames.size() > 0) {
                _this->importBookmarks(paths[0]);
            }
            delete _this->importDialog;
        }
        if (_this->exportOpen && _this->exportDialog->ready()) {
            _this->exportOpen = false;
            std::string path = _this->exportDialog->result();
            if (path != "") {
                _this->exportBookmarks(path);
            }
            delete _this->exportDialog;
        }
    }

    static void fftRedraw(ImGui::WaterFall::FFTRedrawArgs args, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
            for (auto const bm : _this->waterfallBookmarks) {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }

                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.min.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.min.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0) {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.min.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) {
            for (auto const bm : _this->waterfallBookmarks) {
                double centerXpos = args.min.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);

                if (bm.bookmark.frequency >= args.lowFreq && bm.bookmark.frequency <= args.highFreq) {
                    args.window->DrawList->AddLine(ImVec2(centerXpos, args.min.y), ImVec2(centerXpos, args.max.y), IM_COL32(255, 255, 0, 255));
                }

                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.max.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.max.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.min.x, args.max.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.min.x, args.max.x), rectMax.y);

                if (clampedRectMax.x - clampedRectMin.x > 0) {
                    args.window->DrawList->AddRectFilled(clampedRectMin, clampedRectMax, IM_COL32(255, 255, 0, 255));
                }
                if (rectMin.x >= args.min.x && rectMax.x <= args.max.x) {
                    args.window->DrawList->AddText(ImVec2(centerXpos - (nameSize.x / 2), args.max.y - nameSize.y), IM_COL32(0, 0, 0, 255), bm.bookmarkName.c_str());
                }
            }
        }
    }

    bool mouseAlreadyDown = false;
    bool mouseClickedInLabel = false;
    static void fftInput(ImGui::WaterFall::InputHandlerArgs args, void* ctx) {
        FrequencyManagerModule* _this = (FrequencyManagerModule*)ctx;
        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_OFF) { return; }

        if (_this->mouseClickedInLabel) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                _this->mouseClickedInLabel = false;
            }
            gui::waterfall.inputHandled = true;
            return;
        }

        bool inALabel = false;
        WaterfallBookmark hoveredBookmark;
        std::string hoveredBookmarkName;

        if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_TOP) {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--) {
                auto& bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMin.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMin.y + nameSize.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax)) {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }
        else if (_this->bookmarkDisplayMode == BOOKMARK_DISP_MODE_BOTTOM) {
            int count = _this->waterfallBookmarks.size();
            for (int i = count - 1; i >= 0; i--) {
                auto& bm = _this->waterfallBookmarks[i];
                double centerXpos = args.fftRectMin.x + std::round((bm.bookmark.frequency - args.lowFreq) * args.freqToPixelRatio);
                ImVec2 nameSize = ImGui::CalcTextSize(bm.bookmarkName.c_str());
                ImVec2 rectMin = ImVec2(centerXpos - (nameSize.x / 2) - 5, args.fftRectMax.y - nameSize.y);
                ImVec2 rectMax = ImVec2(centerXpos + (nameSize.x / 2) + 5, args.fftRectMax.y);
                ImVec2 clampedRectMin = ImVec2(std::clamp<double>(rectMin.x, args.fftRectMin.x, args.fftRectMax.x), rectMin.y);
                ImVec2 clampedRectMax = ImVec2(std::clamp<double>(rectMax.x, args.fftRectMin.x, args.fftRectMax.x), rectMax.y);

                if (ImGui::IsMouseHoveringRect(clampedRectMin, clampedRectMax)) {
                    inALabel = true;
                    hoveredBookmark = bm;
                    hoveredBookmarkName = bm.bookmarkName;
                    break;
                }
            }
        }

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !inALabel) {
            _this->mouseAlreadyDown = true;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            _this->mouseAlreadyDown = false;
            _this->mouseClickedInLabel = false;
        }

        if (_this->mouseAlreadyDown || !inALabel) { return; }

        gui::waterfall.inputHandled = true;

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            _this->mouseClickedInLabel = true;
            _this->stopScan();
            applyBookmark(hoveredBookmark.bookmark, gui::waterfall.selectedVFO);
        }

        ImGui::BeginTooltip();
        ImGui::TextUnformatted(hoveredBookmarkName.c_str());
        ImGui::Separator();
        ImGui::Text("List: %s", hoveredBookmark.listName.c_str());
        ImGui::Text("Frequency: %s", utils::formatFreq(hoveredBookmark.bookmark.frequency).c_str());
        ImGui::Text("Bandwidth: %s", utils::formatFreq(hoveredBookmark.bookmark.bandwidth).c_str());
        ImGui::Text("Mode: %s", demodModeList[hoveredBookmark.bookmark.mode]);
        ImGui::EndTooltip();
    }

    json exportedBookmarks;
    bool importOpen = false;
    bool exportOpen = false;
    pfd::open_file* importDialog;
    pfd::save_file* exportDialog;

    void importBookmarks(std::string path) {
        std::ifstream fs(path);
        json importBookmarks;
        fs >> importBookmarks;

        if (!importBookmarks.contains("bookmarks")) {
            flog::error("File does not contains any bookmarks");
            return;
        }

        if (!importBookmarks["bookmarks"].is_object()) {
            flog::error("Bookmark attribute is invalid");
            return;
        }

        for (auto const [_name, bm] : importBookmarks["bookmarks"].items()) {
            if (bookmarks.find(_name) != bookmarks.end()) {
                flog::warn("Bookmark with the name '{0}' already exists in list, skipping", _name);
                continue;
            }
            FrequencyBookmark fbm;
            fbm.frequency = bm["frequency"];
            fbm.bandwidth = bm["bandwidth"];
            fbm.mode = bm["mode"];
            fbm.selected = false;
            bookmarks[_name] = fbm;
        }
        saveByName(selectedListName);
        fs.close();
    }

    void exportBookmarks(std::string path) {
        std::ofstream fs(path);
        fs << exportedBookmarks;
        fs.close();
    }

    std::string name;
    bool enabled = true;
    bool createOpen = false;
    bool editOpen = false;
    bool newListOpen = false;
    bool renameListOpen = false;
    bool selectListsOpen = false;

    bool deleteListOpen = false;
    bool deleteBookmarksOpen = false;

    EventHandler<ImGui::WaterFall::FFTRedrawArgs> fftRedrawHandler;
    EventHandler<ImGui::WaterFall::InputHandlerArgs> inputHandler;

    std::map<std::string, FrequencyBookmark> bookmarks;

    std::string editedBookmarkName = "";
    std::string firstEditedBookmarkName = "";
    FrequencyBookmark editedBookmark;

    std::vector<std::string> listNames;
    std::string listNamesTxt = "";
    std::string selectedListName = "";
    int selectedListId = 0;

    std::string editedListName;
    std::string firstEditedListName;

    std::vector<WaterfallBookmark> waterfallBookmarks;
    int bookmarkDisplayMode = 0;

    // Scanner state
    bool scanning = false;
    bool scanReceiving = false;
    bool scanTuning = false;
    bool skipRequested = false;
    int scanIndex = 0;
    float scanLevel = -50.0f;
    int tuningTimeMs = 250;
    int lingerTimeMs = 1000;
    std::string activeScanName = "";
    std::chrono::time_point<std::chrono::high_resolution_clock> lastSignalTime;
    std::chrono::time_point<std::chrono::high_resolution_clock> lastTuneTime;
    std::thread scanThread;
    std::mutex scanMtx;
};

MOD_EXPORT void _INIT_() {
    json def = json({});
    def["selectedList"] = "General";
    def["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    def["lists"]["General"]["showOnWaterfall"] = true;
    def["lists"]["General"]["bookmarks"] = json::object();

    config.setPath(core::args["root"].s() + "/frequency_manager_config.json");
    config.load(def);
    config.enableAutoSave();

    config.acquire();
    if (!config.conf.contains("bookmarkDisplayMode")) {
        config.conf["bookmarkDisplayMode"] = BOOKMARK_DISP_MODE_TOP;
    }
    for (auto [listName, list] : config.conf["lists"].items()) {
        if (list.contains("bookmarks") && list.contains("showOnWaterfall") && list["showOnWaterfall"].is_boolean()) { continue; }
        json newList;
        newList = json::object();
        newList["showOnWaterfall"] = true;
        newList["bookmarks"] = list;
        config.conf["lists"][listName] = newList;
    }
    config.release(true);
}

MOD_EXPORT ModuleManager::Instance* _CREATE_INSTANCE_(std::string name) {
    return new FrequencyManagerModule(name);
}

MOD_EXPORT void _DELETE_INSTANCE_(void* instance) {
    delete (FrequencyManagerModule*)instance;
}

MOD_EXPORT void _END_() {
    config.disableAutoSave();
    config.save();
}
