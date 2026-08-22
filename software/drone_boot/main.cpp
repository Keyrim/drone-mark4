/// @file
/// @brief Entry point of drone_boot. There is nothing to parse and nothing
///        to configure: the bootloader's only input is the boot metadata in
///        flash, so main() builds the app and hands over.

#include "boot_app.hpp"

int main()
{
    mark4::BootApp app;
    app.run();
}
