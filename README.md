# Rooted Robotics - Machine Code Repository
Rooted Robotics Machine Code Repository houses the codebase for various Rooted machines, including the seeder, harvester, and washer. 

It also contains WiFi connectivity utilities and AWS integration code.


## Best Practices
 - After making any changes, always commit and push your changes to the remote repository, if those changes are intended to be shared with others or saved.
 - Use clear and descriptive file names and folder structures to organize your code effectively.
 - Regularly pull updates from the remote repository to keep your local copy in sync with the latest changes made by others (If others are working on the same project).
 - Follow coding standards and conventions to maintain code quality and readability.
 - Document your code and changes thoroughly to facilitate collaboration and future maintenance.
 - Seperate different functionalities into modules or classes to enhance code reusability and maintainability.

## Folder Structure

```
rooted-machines-code/
├── docs/
├── machine-code/
│   ├── seeder/
│   ├── harvester/
│   └── washer/
├── wifi/
│   ├── captive-portal/
│   └── wifi-setup/
└── aws/
```

### Folder Descriptions

**`/docs`**
- Architecture documentation and technical specifications
- Reference materials for system design and implementation
- Contains files like PI-ARCH.md for Raspberry Pi architecture

**`/machine-code`**
- Machine-specific application code organized by machine type
- Each subfolder contains the control logic for a specific Rooted machine:
  - **`seeder/`** - Control software for the seeder machine
  - **`harvester/`** - Control software for the harvester machine
  - **`washer/`** - Control software for the washer machine

**`/wifi`**
- WiFi connectivity and configuration utilities
- **`captive-portal/`** - Captive portal implementation for initial WiFi setup
- **`wifi-setup/`** - WiFi configuration tools and scripts

**`/aws`**
- AWS integration code and configurations
- Cloud connectivity, data sync, and remote management





## High-Level Architecture Diagram

```
┌─────────────────────────────────────────┐
│         Raspberry Pi                     │
│                                          │
│  ┌────────────────────────────────────┐ │
│  │   Application Layer                │ │
│  │   - Control Logic                  │ │
│  │   - Data Processing                │ │
│  │   - UI Management                  │ │
│  └────────────────────────────────────┘ │
│                  │                       │
│  ┌───────────────┴──────────────────┐   │
│  │   Operating System Layer         │   │
│  │   - Hardware Drivers             │   │
│  │   - Communication Interfaces     │   │
│  │   - System Services              │   │
│  └──────────────────────────────────┘   │
│         │                    │           │
└─────────┼────────────────────┼───────────┘
          │                    │
    ┌─────▼─────┐        ┌────▼─────┐
    │ ClearCore │        │  TE-HMI  │
    │  (Motor   │        │ (Touch   │
    │ Controller)│        │  Screen) │
    └───────────┘        └──────────┘
```
