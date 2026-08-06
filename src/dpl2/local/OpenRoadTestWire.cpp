#include "OpenRoadImportDb.h"

#include <tcl.h>

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <ord/OpenRoad.hh>

#include <testFillerRepairCmd.hh>

namespace dpl2::local {
namespace {

std::unique_ptr<TestFillerRepairCmd> fillerRepairCommand;

int importDbCommand(ClientData clientData,
                    Tcl_Interp* interp,
                    int objc,
                    Tcl_Obj* const objv[])
{
  auto* openroad = static_cast<ord::OpenRoad*>(clientData);
  std::vector<std::string> fillerMasters;
  if (objc != 1 && objc != 3) {
    Tcl_SetObjResult(
        interp,
        Tcl_NewStringObj("usage: dpl2_import_db ?-fillers {master ...}?", -1));
    return TCL_ERROR;
  }
  if (objc == 3) {
    const std::string option = Tcl_GetString(objv[1]);
    if (option != "-fillers") {
      Tcl_SetObjResult(
          interp, Tcl_NewStringObj("expected -fillers {master ...}", -1));
      return TCL_ERROR;
    }
    int count = 0;
    Tcl_Obj** values = nullptr;
    if (Tcl_ListObjGetElements(interp, objv[2], &count, &values) != TCL_OK) {
      return TCL_ERROR;
    }
    fillerMasters.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
      fillerMasters.emplace_back(Tcl_GetString(values[index]));
    }
  }
  if (!importOpenRoadDb(openroad->getDb(), fillerMasters, std::cout)) {
    Tcl_SetObjResult(interp, Tcl_NewStringObj("dpl2 import failed", -1));
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, Tcl_NewStringObj("1", -1));
  return TCL_OK;
}

int testFillerRepairCommand(ClientData,
                            Tcl_Interp* interp,
                            int objc,
                            Tcl_Obj* const objv[])
{
  std::vector<std::string> args;
  args.reserve(static_cast<size_t>(std::max(objc - 1, 0)));
  for (int index = 1; index < objc; ++index) {
    args.emplace_back(Tcl_GetString(objv[index]));
  }
  std::string error;
  if (!fillerRepairCommand->run(args, error)) {
    if (error.empty()) {
      error = "test_filler_repair failed";
    }
    Tcl_SetObjResult(interp, Tcl_NewStringObj(error.c_str(), -1));
    return TCL_ERROR;
  }
  Tcl_SetObjResult(interp, Tcl_NewStringObj("1", -1));
  return TCL_OK;
}

}  // namespace

void initOpenRoadTestCommands(ord::OpenRoad* openroad)
{
  Tcl_Interp* interp = openroad->tclInterp();
  fillerRepairCommand = std::make_unique<TestFillerRepairCmd>();
  Tcl_CreateObjCommand(
      interp, "dpl2_import_db", importDbCommand, openroad, nullptr);
  Tcl_CreateObjCommand(interp,
                       "test_filler_repair",
                       testFillerRepairCommand,
                       nullptr,
                       nullptr);
}

}  // namespace dpl2::local
