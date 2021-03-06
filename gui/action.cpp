/*
	Copyright 2013 bigbiff/Dees_Troy TeamWin
	This file is part of TWRP/TeamWin Recovery Project.

	TWRP is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	TWRP is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with TWRP.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <time.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>

#include <string>
#include <sstream>
#include "../partitions.hpp"
#include "../twrp-functions.hpp"
#include "../openrecoveryscript.hpp"

#include "../adb_install.h"
#include "blanktimer.hpp"
#include "../multirom.h"

extern "C" {
#include "../twcommon.h"
#include "../minuitwrp/minui.h"
#include "../variables.h"
#include "../twinstall.h"
#include "cutils/properties.h"
#include "../minadbd/adb.h"

#include "../mrominstaller.h"

int TWinstall_zip(const char* path, int* wipe_cache);
void run_script(const char *str1, const char *str2, const char *str3, const char *str4, const char *str5, const char *str6, const char *str7, int request_confirm);
int gui_console_only();
int gui_start();
};

#include "rapidxml.hpp"
#include "objects.hpp"

extern blanktimer blankTimer;

void curtainClose(void);

GUIAction::GUIAction(xml_node<>* node)
    : Conditional(node)
{
    xml_node<>* child;
    xml_node<>* actions;
    xml_attribute<>* attr;

    mKey = 0;

    if (!node)  return;

    // First, get the action
    actions = node->first_node("actions");
    if (actions)    child = actions->first_node("action");
    else            child = node->first_node("action");

    if (!child) return;

    while (child)
    {
        Action action;

        attr = child->first_attribute("function");
        if (!attr)  return;
    
        action.mFunction = attr->value();
        action.mArg = child->value();
        mActions.push_back(action);

        child = child->next_sibling("action");
    }

    // Now, let's get either the key or region
    child = node->first_node("touch");
    if (child)
    {
        attr = child->first_attribute("key");
        if (attr)
        {
            std::string key = attr->value();
    
            mKey = getKeyByName(key);
        }
        else
        {
            attr = child->first_attribute("x");
            if (!attr)  return;
            mActionX = atol(attr->value());
            attr = child->first_attribute("y");
            if (!attr)  return;
            mActionY = atol(attr->value());
            attr = child->first_attribute("w");
            if (!attr)  return;
            mActionW = atol(attr->value());
            attr = child->first_attribute("h");
            if (!attr)  return;
            mActionH = atol(attr->value());
        }
    }
}

int GUIAction::NotifyTouch(TOUCH_STATE state, int x, int y)
{
    if (state == TOUCH_RELEASE)
        doActions();

    return 0;
}

int GUIAction::NotifyKey(int key)
{
	if (!mKey || key != mKey)
		return 1;

	doActions();
	return 0;
}

int GUIAction::NotifyVarChange(std::string varName, std::string value)
{
    if (varName.empty() && !isConditionValid() && !mKey && !mActionW)
        doActions();

    // This handles notifying the condition system of page start
    if (varName.empty() && isConditionValid())
        NotifyPageSet();

    if ((varName.empty() || IsConditionVariable(varName)) && isConditionValid() && isConditionTrue())
        doActions();

    return 0;
}

void GUIAction::simulate_progress_bar(void)
{
	gui_print("Simulating actions...\n");
	for (int i = 0; i < 5; i++)
	{
		usleep(500000);
		DataManager::SetValue("ui_progress", i * 20);
	}
}

int GUIAction::flash_zip(std::string filename, std::string pageName, const int simulate, int* wipe_cache)
{
    int ret_val = 0;

	DataManager::SetValue("ui_progress", 0);

    if (filename.empty())
    {
        LOGERR("No file specified.\n");
        return -1;
    }

    // We're going to jump to this page first, like a loading page
    gui_changePage(pageName);

    int fd = -1;
    ZipArchive zip;

    if (!PartitionManager.Mount_By_Path(filename, true))
		return -1;

	if (mzOpenZipArchive(filename.c_str(), &zip))
    {
        LOGERR("Unable to open zip file.\n");
        return -1;
    }

    // Check the zip to see if it has a custom installer theme
	const ZipEntry* twrp = mzFindZipEntry(&zip, "META-INF/teamwin/twrp.zip");
    if (twrp != NULL)
    {
        unlink("/tmp/twrp.zip");
        fd = creat("/tmp/twrp.zip", 0666);
    }
    if (fd >= 0 && twrp != NULL && 
        mzExtractZipEntryToFile(&zip, twrp, fd) && 
        !PageManager::LoadPackage("install", "/tmp/twrp.zip", "main"))
    {
        mzCloseZipArchive(&zip);
		PageManager::SelectPackage("install");
        gui_changePage("main");
    }
    else
    {
        // In this case, we just use the default page
        mzCloseZipArchive(&zip);
		gui_changePage(pageName);
    }
    if (fd >= 0)
        close(fd);

	if (simulate) {
		simulate_progress_bar();
	} else {
		ret_val = TWinstall_zip(filename.c_str(), wipe_cache);

		// Now, check if we need to ensure TWRP remains installed...
		struct stat st;
		string result;
		if (stat("/sbin/installTwrp", &st) == 0)
		{
			DataManager::SetValue("tw_operation", "Configuring TWRP");
			DataManager::SetValue("tw_partition", "");
			gui_print("Configuring TWRP...\n");
			if (TWFunc::Exec_Cmd("/sbin/installTwrp reinstall", result) < 0)
			{
				gui_print("Unable to configure TWRP with this kernel.\n");
			}
		}
	}

    // Done
    DataManager::SetValue("ui_progress", 100);
    DataManager::SetValue("ui_progress", 0);
    return ret_val;
}

int GUIAction::doActions()
{
	if (mActions.size() < 1)    return -1;
    if (mActions.size() == 1)
		return doAction(mActions.at(0), 0);

    // For multi-action, we always use a thread
    pthread_t t;
	pthread_attr_t tattr;

	if (pthread_attr_init(&tattr)) {
		LOGERR("Unable to pthread_attr_init\n");
		return -1;
	}
	if (pthread_attr_setdetachstate(&tattr, PTHREAD_CREATE_JOINABLE)) {
		LOGERR("Error setting pthread_attr_setdetachstate\n");
		return -1;
	}
	if (pthread_attr_setscope(&tattr, PTHREAD_SCOPE_SYSTEM)) {
		LOGERR("Error setting pthread_attr_setscope\n");
		return -1;
	}
	/*if (pthread_attr_setstacksize(&tattr, 524288)) {
		LOGERR("Error setting pthread_attr_setstacksize\n");
		return -1;
	}
	*/
	int ret = pthread_create(&t, &tattr, thread_start, this);
    if (ret) {
		LOGERR("Unable to create more threads for actions... continuing in same thread! %i\n", ret);
		thread_start(this);
	} else {
		if (pthread_join(t, NULL)) {
			LOGERR("Error joining threads\n");
		}
	}
	if (pthread_attr_destroy(&tattr)) {
		LOGERR("Failed to pthread_attr_destroy\n");
		return -1;
	}

    return 0;
}

void* GUIAction::thread_start(void *cookie)
{
    GUIAction* ourThis = (GUIAction*) cookie;

	DataManager::SetValue(TW_ACTION_BUSY, 1);

    if (ourThis->mActions.size() > 1)
    {
        std::vector<Action>::iterator iter;
        for (iter = ourThis->mActions.begin(); iter != ourThis->mActions.end(); iter++)
            ourThis->doAction(*iter, 1);
    }
    else
    {
        ourThis->doAction(ourThis->mActions.at(0), 1);
    }
	int check = 0;
	DataManager::GetValue("tw_background_thread_running", check);
	if (check == 0)
		DataManager::SetValue(TW_ACTION_BUSY, 0);
    return NULL;
}

void GUIAction::operation_start(const string operation_name)
{
	DataManager::SetValue(TW_ACTION_BUSY, 1);
	DataManager::SetValue("ui_progress", 0);
	DataManager::SetValue("tw_operation", operation_name);
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 0);
}

void GUIAction::operation_end(const int operation_status, const int simulate)
{
	int simulate_fail;
	DataManager::SetValue("ui_progress", 100);
	if (simulate) {
		DataManager::GetValue(TW_SIMULATE_FAIL, simulate_fail);
		if (simulate_fail != 0)
			DataManager::SetValue("tw_operation_status", 1);
		else
			DataManager::SetValue("tw_operation_status", 0);
	} else {
		if (operation_status != 0) {
			DataManager::SetValue("tw_operation_status", 1);
		}
		else {
			DataManager::SetValue("tw_operation_status", 0);
		}
	}
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
	blankTimer.resetTimerAndUnblank();
}

int GUIAction::doAction(Action action, int isThreaded /* = 0 */)
{
	static string zip_queue[10];
	static int zip_queue_index;
	static pthread_t terminal_command;
	int simulate;

	std::string arg = gui_parse_text(action.mArg);

	std::string function = gui_parse_text(action.mFunction);

	DataManager::GetValue(TW_SIMULATE_ACTIONS, simulate);

	if (function == "reboot")
	{
	        //curtainClose(); this sometimes causes a crash

		sync();
		DataManager::SetValue("tw_gui_done", 1);
		DataManager::SetValue("tw_reboot_arg", arg);

		return 0;
	}
    if (function == "home")
    {
        PageManager::SelectPackage("TWRP");
        gui_changePage("main");
        return 0;
    }

    if (function == "key")
    {
        PageManager::NotifyKey(getKeyByName(arg));
        return 0;
    }

    if (function == "page") {
		std::string page_name = gui_parse_text(arg);
        return gui_changePage(page_name);
	}

    if (function == "reload") {
		int check = 0, ret_val = 0;
		std::string theme_path;

		operation_start("Reload Theme");
		theme_path = DataManager::GetSettingsStoragePath();
		if (PartitionManager.Mount_By_Path(theme_path.c_str(), 1) < 0) {
			LOGERR("Unable to mount %s during reload function startup.\n", theme_path.c_str());
			check = 1;
		}

		theme_path += "/TWRP/theme/ui.zip";
		if (check != 0 || PageManager::ReloadPackage("TWRP", theme_path) != 0)
		{
			// Loading the custom theme failed - try loading the stock theme
			LOGINFO("Attempting to reload stock theme...\n");
			if (PageManager::ReloadPackage("TWRP", "/res/ui.xml"))
			{
				LOGERR("Failed to load base packages.\n");
				ret_val = 1;
			}
		}
        operation_end(ret_val, simulate);
	}

    if (function == "readBackup")
    {
		string Restore_Name;
		DataManager::GetValue("tw_restore", Restore_Name);
		PartitionManager.Set_Restore_Files(Restore_Name);
        return 0;
    }

    if (function == "set")
    {
        if (arg.find('=') != string::npos)
        {
            string varName = arg.substr(0, arg.find('='));
            string value = arg.substr(arg.find('=') + 1, string::npos);

            DataManager::GetValue(value, value);
            DataManager::SetValue(varName, value);
        }
        else
            DataManager::SetValue(arg, "1");
        return 0;
    }
    if (function == "clear")
    {
        DataManager::SetValue(arg, "0");
        return 0;
    }

    if (function == "mount")
    {
        if (arg == "usb")
        {
            DataManager::SetValue(TW_ACTION_BUSY, 1);
			if (!simulate)
				PartitionManager.usb_storage_enable();
			else
				gui_print("Simulating actions...\n");
        }
        else if (!simulate)
        {
            string cmd;
			if (arg == "EXTERNAL")
				PartitionManager.Mount_By_Path(DataManager::GetStrValue(TW_EXTERNAL_MOUNT), true);
			else if (arg == "INTERNAL")
				PartitionManager.Mount_By_Path(DataManager::GetStrValue(TW_INTERNAL_MOUNT), true);
			else
				PartitionManager.Mount_By_Path(arg, true);
        } else
			gui_print("Simulating actions...\n");
        return 0;
    }

    if (function == "umount" || function == "unmount")
    {
        if (arg == "usb")
        {
            if (!simulate)
				PartitionManager.usb_storage_disable();
			else
				gui_print("Simulating actions...\n");
			DataManager::SetValue(TW_ACTION_BUSY, 0);
        }
        else if (!simulate)
        {
            string cmd;
			if (arg == "EXTERNAL")
				PartitionManager.UnMount_By_Path(DataManager::GetStrValue(TW_EXTERNAL_MOUNT), true);
			else if (arg == "INTERNAL")
				PartitionManager.UnMount_By_Path(DataManager::GetStrValue(TW_INTERNAL_MOUNT), true);
			else
				PartitionManager.UnMount_By_Path(arg, true);
        } else
			gui_print("Simulating actions...\n");
        return 0;
    }
	
	if (function == "restoredefaultsettings")
	{
		operation_start("Restore Defaults");
		if (simulate) // Simulated so that people don't accidently wipe out the "simulation is on" setting
			gui_print("Simulating actions...\n");
		else {
			DataManager::ResetDefaults();
			PartitionManager.Update_System_Details();
			PartitionManager.Mount_Current_Storage(true);
		}
		operation_end(0, simulate);
	}
	
	if (function == "copylog")
	{
		operation_start("Copy Log");
		if (!simulate)
		{
			string dst;
			PartitionManager.Mount_Current_Storage(true);
			dst = DataManager::GetCurrentStoragePath() + "/recovery.log";
			TWFunc::copy_file("/tmp/recovery.log", dst.c_str(), 0755);
			sync();
			gui_print("Copied recovery log to %s.\n", DataManager::GetCurrentStoragePath().c_str());
		} else
			simulate_progress_bar();
		operation_end(0, simulate);
		return 0;
	}
	
	if (function == "compute" || function == "addsubtract")
	{
		if (arg.find("+") != string::npos)
        {
            string varName = arg.substr(0, arg.find('+'));
            string string_to_add = arg.substr(arg.find('+') + 1, string::npos);
			int amount_to_add = atoi(string_to_add.c_str());
			int value;

			DataManager::GetValue(varName, value);
            DataManager::SetValue(varName, value + amount_to_add);
			return 0;
        }
		if (arg.find("-") != string::npos)
        {
            string varName = arg.substr(0, arg.find('-'));
            string string_to_subtract = arg.substr(arg.find('-') + 1, string::npos);
			int amount_to_subtract = atoi(string_to_subtract.c_str());
			int value;

			DataManager::GetValue(varName, value);
			value -= amount_to_subtract;
			if (value <= 0)
				value = 0;
            DataManager::SetValue(varName, value);
			return 0;
        }
		if (arg.find("*") != string::npos)
		{
			string varName = arg.substr(0, arg.find('*'));
			string multiply_by_str = gui_parse_text(arg.substr(arg.find('*') + 1, string::npos));
			int multiply_by = atoi(multiply_by_str.c_str());
			int value;

			DataManager::GetValue(varName, value);
			DataManager::SetValue(varName, value*multiply_by);
			return 0;
		}
		if (arg.find("/") != string::npos)
		{
			string varName = arg.substr(0, arg.find('/'));
			string divide_by_str = gui_parse_text(arg.substr(arg.find('/') + 1, string::npos));
			int divide_by = atoi(divide_by_str.c_str());
			int value;

			if(divide_by != 0)
			{
				DataManager::GetValue(varName, value);
				DataManager::SetValue(varName, value/divide_by);
			}
			return 0;
		}
		LOGERR("Unable to perform compute '%s'\n", arg.c_str());
		return -1;
	}
	
	if (function == "setguitimezone")
	{
		string SelectedZone;
		DataManager::GetValue(TW_TIME_ZONE_GUISEL, SelectedZone); // read the selected time zone into SelectedZone
		string Zone = SelectedZone.substr(0, SelectedZone.find(';')); // parse to get time zone
		string DSTZone = SelectedZone.substr(SelectedZone.find(';') + 1, string::npos); // parse to get DST component
		
		int dst;
		DataManager::GetValue(TW_TIME_ZONE_GUIDST, dst); // check wether user chose to use DST
		
		string offset;
		DataManager::GetValue(TW_TIME_ZONE_GUIOFFSET, offset); // pull in offset
		
		string NewTimeZone = Zone;
		if (offset != "0")
			NewTimeZone += ":" + offset;
		
		if (dst != 0)
			NewTimeZone += DSTZone;
		
		DataManager::SetValue(TW_TIME_ZONE_VAR, NewTimeZone);
		DataManager::update_tz_environment_variables();
		return 0;
	}

	if (function == "togglestorage") {
		if (arg == "internal") {
			DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 0);
		} else if (arg == "external") {
			DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 1);
		}
		if (PartitionManager.Mount_Current_Storage(true)) {
			if (arg == "internal") {
				string zip_path, zip_root;
				DataManager::GetValue(TW_ZIP_INTERNAL_VAR, zip_path);
				zip_root = TWFunc::Get_Root_Path(zip_path);
#ifdef RECOVERY_SDCARD_ON_DATA
	#ifndef TW_EXTERNAL_STORAGE_PATH
				if (zip_root != "/sdcard")
					DataManager::SetValue(TW_ZIP_INTERNAL_VAR, "/sdcard");
	#else
				if (strcmp(EXPAND(TW_EXTERNAL_STORAGE_PATH), "/sdcard") == 0) {
					if (zip_root != "/emmc")
						DataManager::SetValue(TW_ZIP_INTERNAL_VAR, "/emmc");
				} else {
					if (zip_root != "/sdcard")
						DataManager::SetValue(TW_ZIP_INTERNAL_VAR, "/sdcard");
				}
	#endif
#else
				if (zip_root != DataManager::GetCurrentStoragePath())
					DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetCurrentStoragePath());
#endif
				// Save the current zip location to the external variable
				DataManager::SetValue(TW_ZIP_EXTERNAL_VAR, DataManager::GetStrValue(TW_ZIP_LOCATION_VAR));
				// Change the current zip location to the internal variable
				DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetStrValue(TW_ZIP_INTERNAL_VAR));
			} else if (arg == "external") {
				string zip_path, zip_root;
				DataManager::GetValue(TW_ZIP_EXTERNAL_VAR, zip_path);
				zip_root = TWFunc::Get_Root_Path(zip_path);
				if (zip_root != DataManager::GetCurrentStoragePath()) {
					DataManager::SetValue(TW_ZIP_EXTERNAL_VAR, DataManager::GetCurrentStoragePath());
				}
				// Save the current zip location to the internal variable
				DataManager::SetValue(TW_ZIP_INTERNAL_VAR, DataManager::GetStrValue(TW_ZIP_LOCATION_VAR));
				// Change the current zip location to the external variable
				DataManager::SetValue(TW_ZIP_LOCATION_VAR, DataManager::GetStrValue(TW_ZIP_EXTERNAL_VAR));
			}
		} else {
			// We weren't able to toggle for some reason, restore original setting
			if (arg == "internal") {
				DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 1);
			} else if (arg == "external") {
				DataManager::SetValue(TW_USE_EXTERNAL_STORAGE, 0);
			}
		}
		return 0;
	}
	
	if (function == "overlay")
        return gui_changeOverlay(arg);

	if (function == "queuezip")
    {
        if (zip_queue_index >= 10) {
			gui_print("Maximum zip queue reached!\n");
			return 0;
		}
		DataManager::GetValue("tw_filename", zip_queue[zip_queue_index]);
		if (strlen(zip_queue[zip_queue_index].c_str()) > 0) {
			zip_queue_index++;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}

	if (function == "cancelzip")
    {
        if (zip_queue_index <= 0) {
			gui_print("Minimum zip queue reached!\n");
			return 0;
		} else {
			zip_queue_index--;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		}
		return 0;
	}

	if (function == "queueclear")
	{
		zip_queue_index = 0;
		DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);
		return 0;
	}

	if (function == "sleep")
	{
		operation_start("Sleep");
		usleep(atoi(arg.c_str()));
		operation_end(0, simulate);
		return 0;
	}

	if (function == "multirom")
	{
		if(MultiROM::folderExists())
			return gui_changePage("multirom_main");
		else
		{
			DataManager::SetValue("tw_mrom_title", "MultiROM is not installed!");
			DataManager::SetValue("tw_mrom_text1", "/data/media/multirom not found.");
			DataManager::SetValue("tw_mrom_text2", "/data/media/0/multirom not found.");
			DataManager::SetValue("tw_mrom_back", "advanced");
			return gui_changePage("multirom_msg");
		}
	}

	if (function == "multirom_list")
	{
		MultiROM::setRomsPath(INTERNAL_MEM_LOC_TXT);
		DataManager::SetValue("tw_multirom_folder", MultiROM::getRomsPath());
		DataManager::SetValue("tw_multirom_install_loc", INTERNAL_MEM_LOC_TXT);
		return gui_changePage("multirom_list");
	}

	if (function == "multirom_rename")
	{
		MultiROM::move(DataManager::GetStrValue("tw_multirom_rom_name"), arg);
		return gui_changePage("multirom_list");
	}

	if (function == "multirom_manage")
	{
		int type = MultiROM::getType(DataManager::GetStrValue("tw_multirom_rom_name"));
		DataManager::SetValue("tw_multirom_is_android", (M(type) & MASK_ANDROID) != 0);
		return gui_changePage("multirom_manage");
	}

	if (function == "multirom_settings")
	{
		MultiROM::config cfg = MultiROM::loadConfig();
		DataManager::SetValue("tw_multirom_enable_auto_boot", cfg.auto_boot_seconds > 0);
		if(cfg.auto_boot_seconds <= 0)
			DataManager::SetValue("tw_multirom_delay", 5);
		else
			DataManager::SetValue("tw_multirom_delay", cfg.auto_boot_seconds);
		DataManager::SetValue("tw_multirom_current", cfg.current_rom);
		DataManager::SetValue("tw_multirom_auto_boot_rom", cfg.auto_boot_rom);
		DataManager::SetValue("tw_multirom_colors", cfg.colors);
		DataManager::SetValue("tw_multirom_brightness", cfg.brightness);
		DataManager::SetValue("tw_multirom_enable_adb", cfg.enable_adb);

		DataManager::SetValue("tw_multirom_roms", MultiROM::listRoms());
		return gui_changePage("multirom_settings");
	}

	if (function == "multirom_settings_save")
	{
		MultiROM::config cfg;
		cfg.current_rom = DataManager::GetStrValue("tw_multirom_current");
		if(DataManager::GetIntValue("tw_multirom_enable_auto_boot"))
			cfg.auto_boot_seconds = DataManager::GetIntValue("tw_multirom_delay");
		else
			cfg.auto_boot_seconds = 0;
		cfg.auto_boot_rom = DataManager::GetStrValue("tw_multirom_auto_boot_rom");
		cfg.colors = DataManager::GetIntValue("tw_multirom_colors");
		cfg.brightness = DataManager::GetIntValue("tw_multirom_brightness");
		cfg.enable_adb = DataManager::GetIntValue("tw_multirom_enable_adb");

		MultiROM::saveConfig(cfg);
		return gui_changePage("multirom_main");
	}

	if (function == "multirom_add")
	{
		DataManager::SetValue("tw_multirom_install_loc_list", MultiROM::listInstallLocations());
		DataManager::SetValue("tw_multirom_install_loc", INTERNAL_MEM_LOC_TXT);
		return gui_changePage("multirom_add");
	}

	if (function == "multirom_add_second")
	{
		if(DataManager::GetIntValue("tw_multirom_type") == 1)
			return gui_changePage("multirom_add_source");
		else
			return gui_changePage("multirom_add_select");
	}

	if (function == "multirom_add_file_selected")
	{
		std::string loc = DataManager::GetStrValue("tw_multirom_install_loc");
		bool images = loc.compare(INTERNAL_MEM_LOC_TXT) != 0 && loc.find("(ext") == std::string::npos;
		int type = DataManager::GetIntValue("tw_multirom_type");

		MultiROM::clearBaseFolders();

		if(type == 1 || type == 2)
		{
			if(type == 1)
			{
				MultiROM::addBaseFolder("data", 150, 1024);
				MultiROM::addBaseFolder("system", 450, 640);
				MultiROM::addBaseFolder("cache", 50, 436);
			}
			else
				MultiROM::addBaseFolder("root", 2000, 4095);

			MultiROM::updateImageVariables();

			if(images)
				return gui_changePage("multirom_add_image_size");
			else
				return gui_changePage("multirom_add_start_process");
		}
		else if(type == 3)
		{
			DataManager::SetValue("tw_mrom_back", "multirom_add");
			DataManager::SetValue("tw_mrom_text2", "");

			std:string ex;
			MROMInstaller *i = new MROMInstaller();

			DataManager::SetValue("tw_mrom_title", "Bad installer");
			if(!(ex = i->open(DataManager::GetStrValue("tw_filename"))).empty())
				return i->destroyWithErrorMsg(ex);

			DataManager::SetValue("tw_mrom_title", "Unsupported device");
			if(!(ex = i->checkDevices()).empty())
				return i->destroyWithErrorMsg(ex);

			DataManager::SetValue("tw_mrom_title", "Old MultiROM");
			if(!(ex = i->checkVersion()).empty())
				return i->destroyWithErrorMsg(ex);

			DataManager::SetValue("tw_mrom_title", "Unsupported install location");
			if(!(ex = i->setInstallLoc(loc, images)).empty())
				return i->destroyWithErrorMsg(ex);
			
			if(!(ex = i->parseBaseFolders(loc.find("ntfs") != std::string::npos)).empty())
				return i->destroyWithErrorMsg(ex);

			MultiROM::updateImageVariables();
			MultiROM::setInstaller(i);

			if(images)
				return gui_changePage("multirom_add_image_size");
			else
				return gui_changePage("multirom_add_start_process");
		}
	}

	if (function == "multirom_change_img_size")
	{
		DataManager::SetValue("tw_multirom_image_too_small", 0);
		DataManager::SetValue("tw_multirom_image_too_big", 0);
		DataManager::SetValue("tw_multirom_image_name", arg);

		base_folder *b = MultiROM::getBaseFolder(arg);
		if(b != NULL)
			DataManager::SetValue("tw_multirom_image_size", b->size);

		return gui_changePage("multirom_change_img_size");
	}

	if (function == "multirom_change_img_size_act")
	{
		int value = DataManager::GetIntValue("tw_multirom_image_size");

		base_folder *b = MultiROM::getBaseFolder(DataManager::GetStrValue("tw_multirom_image_name"));
		if(!b)
			return gui_changePage("multirom_add_image_size");

		DataManager::SetValue("tw_multirom_image_too_small", 0);
		DataManager::SetValue("tw_multirom_image_too_big", 0);

		if(value < b->min_size)
		{
			DataManager::SetValue("tw_multirom_image_too_small", 1);
			DataManager::SetValue("tw_multirom_min_size", b->min_size);
			return gui_changePage("multirom_change_img_size");
		}

		if(value > 4095 &&
			DataManager::GetStrValue("tw_multirom_install_loc").find("(vfat") != std::string::npos)
		{
			DataManager::SetValue("tw_multirom_image_too_big", 1);
			return gui_changePage("multirom_change_img_size");
		}

		b->size = value;
		MultiROM::updateImageVariables();
		return gui_changePage("multirom_add_image_size");
	}

	if (function == "multirom_set_list_loc")
	{
		DataManager::SetValue("tw_multirom_install_loc_list", MultiROM::listInstallLocations());
		return gui_changePage("multirom_set_list_loc");
	}

	if (function == "multirom_list_loc_selected")
	{
		std::string loc = DataManager::GetStrValue("tw_multirom_install_loc");
		MultiROM::setRomsPath(loc);
		DataManager::SetValue("tw_multirom_folder", MultiROM::getRomsPath());
		return gui_changePage("multirom_list");
	}

    if (isThreaded)
    {
		if (function == "timeout")
		{
			blankTimer.blankScreen();
			return 0;
		}
	
		if (function == "multirom_delete")
		{
			int op_status = 0;
			operation_start("Delete ROM");
			if(!MultiROM::erase(DataManager::GetStrValue("tw_multirom_rom_name")))
				op_status = 1;
			PartitionManager.Update_System_Details();
			operation_end(op_status, simulate);
			return 0;
		}

		if (function == "multirom_flash_zip")
		{
			operation_start("Flashing");
			int op_status = 0;

			if(!MultiROM::flashZip(DataManager::GetStrValue("tw_multirom_rom_name"),
									DataManager::GetStrValue("tw_filename")))
			{
				op_status = 1;
			}

			PartitionManager.Update_System_Details();
			operation_end(op_status, simulate);
		}

		if (function == "multirom_inject")
		{
			operation_start("Injecting");
			int op_status = 0;
			std::string path = DataManager::GetStrValue("tw_filename");
			if(DataManager::GetIntValue("tw_multirom_add_bootimg"))
				op_status = MultiROM::copyBoot(path, DataManager::GetStrValue("tw_multirom_rom_name"));

			if(!op_status)
				op_status = !MultiROM::injectBoot(path);
			operation_end(op_status, simulate);
		}

		if (function == "multirom_inject_curr_boot")
		{
			operation_start("Injecting");
			int op_status = !MultiROM::folderExists();
			if(op_status)
				gui_print("MultiROM is not installed!\n");
			else
				op_status = !MultiROM::injectBoot("/dev/block/mmcblk0p6");
			operation_end(op_status, simulate);
		}

		if (function == "multirom_add_rom")
		{
			operation_start("Installing");

			int op_status = !MultiROM::addROM(DataManager::GetStrValue("tw_filename"),
											  DataManager::GetIntValue("tw_multirom_type"),
											  DataManager::GetStrValue("tw_multirom_install_loc"));
			PartitionManager.Update_System_Details();
			operation_end(op_status, simulate);
		}

		if (function == "multirom_ubuntu_patch_init")
		{
			operation_start("Patching");
			int op_status = !MultiROM::patchInit(DataManager::GetStrValue("tw_multirom_rom_name"));
			operation_end(op_status, simulate);
		}

		if (function == "multirom_wipe")
		{
			operation_start("Wiping");
			int op_status = !MultiROM::wipe(DataManager::GetStrValue("tw_multirom_rom_name"),
											DataManager::GetStrValue("tw_multirom_wipe"));
			operation_end(op_status, simulate);
		}

		if (function == "multirom_disable_flash_kernel")
		{
			operation_start("working");
			int op_status = !MultiROM::disableFlashKernelAct(DataManager::GetStrValue("tw_multirom_rom_name"),
															 DataManager::GetStrValue("tw_multirom_install_loc"));
			operation_end(op_status, simulate);
		}

        if (function == "fileexists")
		{
			struct stat st;
			string newpath = arg + "/.";

			operation_start("FileExists");
			if (stat(arg.c_str(), &st) == 0 || stat(newpath.c_str(), &st) == 0)
				operation_end(0, simulate);
			else
				operation_end(1, simulate);
		}

		if (function == "flash")
        {
			int i, ret_val = 0, wipe_cache = 0;

			for (i=0; i<zip_queue_index; i++) {
				operation_start("Flashing");
		        DataManager::SetValue("tw_filename", zip_queue[i]);
		        DataManager::SetValue(TW_ZIP_INDEX, (i + 1));

				ret_val = flash_zip(zip_queue[i], arg, simulate, &wipe_cache);
				if (ret_val != 0) {
					gui_print("Error flashing zip '%s'\n", zip_queue[i].c_str());
					i = 10; // Error flashing zip - exit queue
					ret_val = 1;
				}
			}
			zip_queue_index = 0;
			DataManager::SetValue(TW_ZIP_QUEUE_COUNT, zip_queue_index);

			if (wipe_cache)
				PartitionManager.Wipe_By_Path("/cache");
			string result;
			if (DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) {
				operation_start("ReinjectTWRP");
				gui_print("Injecting TWRP into boot image...\n");
				if (simulate) {
					simulate_progress_bar();
				} else {
					TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
					if (Boot == NULL || Boot->Current_File_System != "emmc")
						TWFunc::Exec_Cmd("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash", result);
					else {
						string injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash bd=" + Boot->Actual_Block_Device;
						TWFunc::Exec_Cmd(injectcmd, result);
					}
					gui_print("TWRP injection complete.\n");
				}
			}
			PartitionManager.Update_System_Details();
			operation_end(ret_val, simulate);
            return 0;
        }
        if (function == "wipe")
        {
            operation_start("Format");
            DataManager::SetValue("tw_partition", arg);

			int ret_val = false;

			if (simulate) {
				simulate_progress_bar();
			} else {
				if (arg == "data")
					ret_val = PartitionManager.Factory_Reset();
				else if (arg == "battery")
					ret_val = PartitionManager.Wipe_Battery_Stats();
				else if (arg == "rotate")
					ret_val = PartitionManager.Wipe_Rotate_Data();
				else if (arg == "dalvik")
					ret_val = PartitionManager.Wipe_Dalvik_Cache();
				else if (arg == "DATAMEDIA") {
					ret_val = PartitionManager.Format_Data();
				} else if (arg == "INTERNAL") {
					int has_datamedia, dual_storage;

					DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
					if (has_datamedia) {
						ret_val = PartitionManager.Wipe_Media_From_Data();
					} else {
						ret_val = PartitionManager.Wipe_By_Path(DataManager::GetSettingsStoragePath());
					}
				} else if (arg == "EXTERNAL") {
					string External_Path;

					DataManager::GetValue(TW_EXTERNAL_PATH, External_Path);
					ret_val = PartitionManager.Wipe_By_Path(External_Path);
				} else if (arg == "ANDROIDSECURE") {
					ret_val = PartitionManager.Wipe_Android_Secure();
				} else if (arg == "LIST") {
					string Wipe_List, wipe_path;
					bool skip = false;
					ret_val = true;
					TWPartition* wipe_part = NULL;

					DataManager::GetValue("tw_wipe_list", Wipe_List);
					LOGINFO("wipe list '%s'\n", Wipe_List.c_str());
					if (!Wipe_List.empty()) {
						size_t start_pos = 0, end_pos = Wipe_List.find(";", start_pos);
						while (end_pos != string::npos && start_pos < Wipe_List.size()) {
							wipe_path = Wipe_List.substr(start_pos, end_pos - start_pos);
							LOGINFO("wipe_path '%s'\n", wipe_path.c_str());
							if (wipe_path == "/and-sec") {
								if (!PartitionManager.Wipe_Android_Secure()) {
									LOGERR("Unable to wipe android secure\n");
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							} else if (wipe_path == "DALVIK") {
								if (!PartitionManager.Wipe_Dalvik_Cache()) {
									LOGERR("Failed to wipe dalvik\n");
									ret_val = false;
									break;
								} else {
									skip = true;
								}
							}
							if (!skip) {
								if (!PartitionManager.Wipe_By_Path(wipe_path)) {
									LOGERR("Unable to wipe '%s'\n", wipe_path.c_str());
									ret_val = false;
									break;
								} else if (wipe_path == DataManager::GetSettingsStoragePath()) {
									arg = wipe_path;
								}
							} else {
								skip = false;
							}
							start_pos = end_pos + 1;
							end_pos = Wipe_List.find(";", start_pos);
						}
					}
				} else
					ret_val = PartitionManager.Wipe_By_Path(arg);

				if (arg == DataManager::GetSettingsStoragePath()) {
					// If we wiped the settings storage path, recreate the TWRP folder and dump the settings
					string Storage_Path = DataManager::GetSettingsStoragePath();

					if (PartitionManager.Mount_By_Path(Storage_Path, true)) {
						LOGINFO("Making TWRP folder and saving settings.\n");
						Storage_Path += "/TWRP";
						mkdir(Storage_Path.c_str(), 0777);
						DataManager::Flush();
					} else {
						LOGERR("Unable to recreate TWRP folder and save settings.\n");
					}
				}
			}
			PartitionManager.Update_System_Details();
			if (ret_val)
				ret_val = 0; // 0 is success
			else
				ret_val = 1; // 1 is failure
            operation_end(ret_val, simulate);
            return 0;
        }
		if (function == "refreshsizes")
		{
			operation_start("Refreshing Sizes");
			if (simulate) {
				simulate_progress_bar();
			} else
				PartitionManager.Update_System_Details();
			operation_end(0, simulate);
		}
        if (function == "nandroid")
        {
            operation_start("Nandroid");
			int ret = 0;

			if (simulate) {
				DataManager::SetValue("tw_partition", "Simulation");
				simulate_progress_bar();
			} else {
				if (arg == "backup") {
					string Backup_Name;
					DataManager::GetValue(TW_BACKUP_NAME, Backup_Name);
					if (Backup_Name == "(Current Date)" || Backup_Name == "0" || Backup_Name == "(" || PartitionManager.Check_Backup_Name(true) == 0) {
						ret = PartitionManager.Run_Backup();
					}
					else {
						operation_end(1, simulate);
						return -1;
					}
					DataManager::SetValue(TW_BACKUP_NAME, "(Current Date)");
				} else if (arg == "restore") {
					string Restore_Name;
					DataManager::GetValue("tw_restore", Restore_Name);
					ret = PartitionManager.Run_Restore(Restore_Name);
				} else {
					operation_end(1, simulate);
					return -1;
				}
			}
			if (ret == false)
				ret = 1; // 1 for failure
			else
				ret = 0; // 0 for success
            	operation_end(ret, simulate);
		return 0;
        }
		if (function == "fixpermissions")
		{
			operation_start("Fix Permissions");
            LOGINFO("fix permissions started!\n");
			if (simulate) {
				simulate_progress_bar();
			} else {
				int op_status = PartitionManager.Fix_Permissions();
				if (op_status != 0)
					op_status = 1; // failure
				operation_end(op_status, simulate);
			}
			return 0;
		}
        if (function == "dd")
        {
            operation_start("imaging");

			if (simulate) {
				simulate_progress_bar();
			} else {
				string result;
				string cmd = "dd " + arg;
				TWFunc::Exec_Cmd(cmd, result);
			}
            operation_end(0, simulate);
            return 0;
        }
		if (function == "partitionsd")
		{
			operation_start("Partition SD Card");
			int ret_val = 0;

			if (simulate) {
				simulate_progress_bar();
			} else {
				int allow_partition;
				DataManager::GetValue(TW_ALLOW_PARTITION_SDCARD, allow_partition);
				if (allow_partition == 0) {
					gui_print("This device does not have a real SD Card!\nAborting!\n");
				} else {
					if (!PartitionManager.Partition_SDCard())
						ret_val = 1; // failed
				}
			}
			operation_end(ret_val, simulate);
			return 0;
		}
		if (function == "installhtcdumlock")
		{
			operation_start("Install HTC Dumlock");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::install_htc_dumlock();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockrestoreboot")
		{
			operation_start("HTC Dumlock Restore Boot");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::htc_dumlock_restore_original_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "htcdumlockreflashrecovery")
		{
			operation_start("HTC Dumlock Reflash Recovery");
			if (simulate) {
				simulate_progress_bar();
			} else
				TWFunc::htc_dumlock_reflash_recovery_to_boot();

			operation_end(0, simulate);
			return 0;
		}
		if (function == "cmd")
		{
			int op_status = 0;
			string result;

			operation_start("Command");
			LOGINFO("Running command: '%s'\n", arg.c_str());
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = TWFunc::Exec_Cmd(arg, result);
				if (op_status != 0)
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "terminalcommand")
		{
			int op_status = 0;
			string cmdpath, command;

			DataManager::GetValue("tw_terminal_location", cmdpath);
			operation_start("CommandOutput");
			gui_print("%s # %s\n", cmdpath.c_str(), arg.c_str());
			if (simulate) {
				simulate_progress_bar();
				operation_end(op_status, simulate);
			} else {
				command = "cd \"" + cmdpath + "\" && " + arg + " 2>&1";;
				LOGINFO("Actual command is: '%s'\n", command.c_str());
				DataManager::SetValue("tw_terminal_command_thread", command);
				DataManager::SetValue("tw_terminal_state", 1);
				DataManager::SetValue("tw_background_thread_running", 1);
				op_status = pthread_create(&terminal_command, NULL, command_thread, NULL);
				if (op_status != 0) {
					LOGERR("Error starting terminal command thread, %i.\n", op_status);
					DataManager::SetValue("tw_terminal_state", 0);
					DataManager::SetValue("tw_background_thread_running", 0);
					operation_end(1, simulate);
				}
			}
			return 0;
		}
		if (function == "killterminal")
		{
			int op_status = 0;

			LOGINFO("Sending kill command...\n");
			operation_start("KillCommand");
			DataManager::SetValue("tw_operation_status", 0);
			DataManager::SetValue("tw_operation_state", 1);
			DataManager::SetValue("tw_terminal_state", 0);
			DataManager::SetValue("tw_background_thread_running", 0);
			DataManager::SetValue(TW_ACTION_BUSY, 0);
			return 0;
		}
		if (function == "reinjecttwrp")
		{
			int op_status = 0;
			string result;
			operation_start("ReinjectTWRP");
			gui_print("Injecting TWRP into boot image...\n");
			if (simulate) {
				simulate_progress_bar();
			} else {
				TWFunc::Exec_Cmd("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash", result);
				gui_print("TWRP injection complete.\n");
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "checkbackupname")
		{
			int op_status = 0;

			operation_start("CheckBackupName");
			if (simulate) {
				simulate_progress_bar();
			} else {
				op_status = PartitionManager.Check_Backup_Name(true);
				if (op_status != 0)
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "decrypt")
		{
			int op_status = 0;

			operation_start("Decrypt");
			if (simulate) {
				simulate_progress_bar();
			} else {
				string Password;
				DataManager::GetValue("tw_crypto_password", Password);
				op_status = PartitionManager.Decrypt_Device(Password);
				if (op_status != 0)
					op_status = 1;
				else {
					int load_theme = 1;

					DataManager::SetValue(TW_IS_ENCRYPTED, 0);

					if (load_theme) {
						int has_datamedia;

						// Check for a custom theme and load it if exists
						DataManager::GetValue(TW_HAS_DATA_MEDIA, has_datamedia);
						if (has_datamedia != 0) {
							struct stat st;
							int check = 0;
							std::string theme_path;

							theme_path = DataManager::GetSettingsStoragePath();
							if (PartitionManager.Mount_By_Path(theme_path.c_str(), 1) < 0) {
								LOGERR("Unable to mount %s during reload function startup.\n", theme_path.c_str());
								check = 1;
							}

							theme_path += "/TWRP/theme/ui.zip";
							if (check == 0 && stat(theme_path.c_str(), &st) == 0) {
								if (PageManager::ReloadPackage("TWRP", theme_path) != 0)
								{
									// Loading the custom theme failed - try loading the stock theme
									LOGINFO("Attempting to reload stock theme...\n");
									if (PageManager::ReloadPackage("TWRP", "/res/ui.xml"))
									{
										LOGERR("Failed to load base packages.\n");
									}
								}
							}
						}
					}
				}
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "adbsideload")
		{
			int ret = 0;

			operation_start("Sideload");
			if (simulate) {
				simulate_progress_bar();
			} else {
				int wipe_cache = 0;
				int wipe_dalvik = 0;
				string result, Sideload_File;

				if (!PartitionManager.Mount_Current_Storage(true)) {
					operation_end(1, simulate);
					return 0;
				}
				Sideload_File = DataManager::GetCurrentStoragePath() + "/sideload.zip";
				if (TWFunc::Path_Exists(Sideload_File)) {
					unlink(Sideload_File.c_str());
				}
				gui_print("Starting ADB sideload feature...\n");
				DataManager::GetValue("tw_wipe_dalvik", wipe_dalvik);
				ret = apply_from_adb(Sideload_File.c_str());
				DataManager::SetValue("tw_has_cancel", 0); // Remove cancel button from gui now that the zip install is going to start
				if (ret != 0) {
					ret = 1; // failure
				} else if (TWinstall_zip(Sideload_File.c_str(), &wipe_cache) == 0) {
					if (wipe_cache || DataManager::GetIntValue("tw_wipe_cache"))
						PartitionManager.Wipe_By_Path("/cache");
					if (wipe_dalvik)
						PartitionManager.Wipe_Dalvik_Cache();
				} else {
					ret = 1; // failure
				}
				if (DataManager::GetIntValue(TW_HAS_INJECTTWRP) == 1 && DataManager::GetIntValue(TW_INJECT_AFTER_ZIP) == 1) {
					operation_start("ReinjectTWRP");
					gui_print("Injecting TWRP into boot image...\n");
					if (simulate) {
						simulate_progress_bar();
					} else {
						TWPartition* Boot = PartitionManager.Find_Partition_By_Path("/boot");
						if (Boot == NULL || Boot->Current_File_System != "emmc")
							TWFunc::Exec_Cmd("injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash", result);
						else {
							string injectcmd = "injecttwrp --dump /tmp/backup_recovery_ramdisk.img /tmp/injected_boot.img --flash bd=" + Boot->Actual_Block_Device;
							TWFunc::Exec_Cmd(injectcmd, result);
						}
						gui_print("TWRP injection complete.\n");
					}
				}
			}
			operation_end(ret, simulate);
			return 0;
		}
		if (function == "adbsideloadcancel")
		{
			int child_pid;
			char child_prop[PROPERTY_VALUE_MAX];
			string Sideload_File;
			Sideload_File = DataManager::GetCurrentStoragePath() + "/sideload.zip";
			unlink(Sideload_File.c_str());
			property_get("tw_child_pid", child_prop, "error");
			if (strcmp(child_prop, "error") == 0) {
				LOGERR("Unable to get child ID from prop\n");
				return 0;
			}
			child_pid = atoi(child_prop);
			gui_print("Cancelling ADB sideload...\n");
			kill(child_pid, SIGTERM);
			DataManager::SetValue("tw_page_done", "1"); // For OpenRecoveryScript support
			return 0;
		}
		if (function == "openrecoveryscript") {
			operation_start("OpenRecoveryScript");
			if (simulate) {
				simulate_progress_bar();
			} else {
				// Check for the SCRIPT_FILE_TMP first as these are AOSP recovery commands
				// that we converted to ORS commands during boot in recovery.cpp.
				// Run those first.
				int reboot = 0;
				if (TWFunc::Path_Exists(SCRIPT_FILE_TMP)) {
					gui_print("Processing AOSP recovery commands...\n");
					if (OpenRecoveryScript::run_script_file() == 0) {
						reboot = 1;
					}
				}
				// Check for the ORS file in /cache and attempt to run those commands.
				if (OpenRecoveryScript::check_for_script_file()) {
					gui_print("Processing OpenRecoveryScript file...\n");
					if (OpenRecoveryScript::run_script_file() == 0) {
						reboot = 1;
					}
				}
				if (reboot) {
					usleep(2000000); // Sleep for 2 seconds before rebooting
					TWFunc::tw_reboot(rb_system);
				} else {
					DataManager::SetValue("tw_page_done", 1);
				}
			}
		}
		if (function == "installsu")
		{
			int op_status = 0;

			operation_start("Install SuperSU");
			if (simulate) {
				simulate_progress_bar();
			} else {
				if (!TWFunc::Install_SuperSU())
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
		if (function == "fixsu")
		{
			int op_status = 0;

			operation_start("Fixing Superuser Permissions");
			if (simulate) {
				simulate_progress_bar();
			} else {
				if (!TWFunc::Fix_su_Perms())
					op_status = 1;
			}

			operation_end(op_status, simulate);
			return 0;
		}
    }
    else
    {
        pthread_t t;
        pthread_create(&t, NULL, thread_start, this);
        return 0;
    }
    return -1;
}

int GUIAction::getKeyByName(std::string key)
{
    if (key == "home")          return KEY_HOME;
    else if (key == "menu")     return KEY_MENU;
    else if (key == "back")     return KEY_BACK;
    else if (key == "search")   return KEY_SEARCH;
    else if (key == "voldown")  return KEY_VOLUMEDOWN;
    else if (key == "volup")    return KEY_VOLUMEUP;
    else if (key == "power") {
		int ret_val;
		DataManager::GetValue(TW_POWER_BUTTON, ret_val);
		if (!ret_val)
			return KEY_POWER;
		else
			return ret_val;
	}

    return atol(key.c_str());
}

void* GUIAction::command_thread(void *cookie)
{
	string command;
	FILE* fp;
	char line[512];

	DataManager::GetValue("tw_terminal_command_thread", command);
	fp = popen(command.c_str(), "r");
	if (fp == NULL) {
		LOGERR("Error opening command to run.\n");
	} else {
		int fd = fileno(fp), has_data = 0, check = 0, keep_going = -1, bytes_read = 0;
		struct timeval timeout;
		fd_set fdset;

		while(keep_going)
		{
			FD_ZERO(&fdset);
			FD_SET(fd, &fdset);
			timeout.tv_sec = 0;
			timeout.tv_usec = 400000;
			has_data = select(fd+1, &fdset, NULL, NULL, &timeout);
			if (has_data == 0) {
				// Timeout reached
				DataManager::GetValue("tw_terminal_state", check);
				if (check == 0) {
					keep_going = 0;
				}
			} else if (has_data < 0) {
				// End of execution
				keep_going = 0;
			} else {
				// Try to read output
				memset(line, 0, sizeof(line));
				bytes_read = read(fd, line, sizeof(line));
				if (bytes_read > 0)
					gui_print("%s", line); // Display output
				else
					keep_going = 0; // Done executing
			}
		}
		fclose(fp);
	}
	DataManager::SetValue("tw_operation_status", 0);
	DataManager::SetValue("tw_operation_state", 1);
	DataManager::SetValue("tw_terminal_state", 0);
	DataManager::SetValue("tw_background_thread_running", 0);
	DataManager::SetValue(TW_ACTION_BUSY, 0);
	return NULL;
}
