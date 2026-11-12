#pragma once
#include "ios.h"

namespace ios {
std::shared_ptr<Device> make_dev_fs(const std::string& root);
}
