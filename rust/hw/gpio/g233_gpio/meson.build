_libg233_gpio_rs = static_library(
  'g233_gpio',
  files('src/lib.rs'),
  override_options: ['rust_std=2021', 'build.rust_std=2021'],
  rust_abi: 'rust',
  link_with: [
    _bql_rs,
    _qom_rs,
    _system_rs,
    _hwcore_rs,
    _util_rs,
  ],
  dependencies: [
    common_rs,
  ],
)

rust_devices_ss.add(when: 'CONFIG_X_G233_GPIO_RUST', if_true: [declare_dependency(
  link_whole: [_libg233_gpio_rs],
  variables: {'crate': 'g233_gpio'},
)])
