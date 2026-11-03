# nemu-link用のruntime Macro

このmoduleはnemu-link用のruntime Macro slotを提供し、Dynamic Settingsからmacro programを編集できるようにします。

本moduleはnemu-link用のZMK構成を前提とします。

## 導入（最小構成）

### west.yml

この例は既存の`west.yml`へ追記します。`sekigon-gonnoc` remoteが既にある場合は重ねて定義しません。nemu-link構成では、互換性のあるZMK forkと`zmk-feature-nemu-link`もmanifestに必要です。

```yaml
manifest:
  remotes:
    - name: sekigon-gonnoc
      url-base: https://github.com/sekigon-gonnoc
  projects:
    - name: zmk-feature-dynamic-settings
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
    - name: zmk-feature-dynamic-macro
      remote: sekigon-gonnoc
      revision: <compatible-release-tag-or-commit>
```

revisionには利用するZMK / nemu-link moduleと互換性のあるtagまたはcommitを指定し、manifest更新後に`west update`を実行します。

### .conf

```conf
CONFIG_ZMK_STUDIO=y
CONFIG_ZMK_STUDIO_RPC=y
CONFIG_ZMK_DYNAMIC_SETTINGS=y
CONFIG_SETTINGS_NVS=y
```

`CONFIG_SETTINGS_NVS`の代わりに、対象boardで利用するSettings backendを有効にできます。 manager nodeとbehavior nodeがあると`CONFIG_ZMK_DYNAMIC_MACRO`が有効になります。

### overlay

manager nodeを1つ、利用するslotごとにbehavior nodeを宣言します。behavior labelをkeymapのbindingに指定してslotを実行します。

```dts
/ {
    macro_manager: macro_manager {
        compatible = "sekigon,dynamic-macro-manager";
    };

    macro_slot0: macro_slot0 {
        compatible = "sekigon,dynamic-behavior-macro";
        #binding-cells = <0>;
    };
};
```

keymapではbehavior labelをbindingに指定してslotを実行します（例: `&macro_slot0`）。

## 追加設定

`CONFIG_ZMK_DYNAMIC_MACRO_MAX_SLOTS`は生成するslot数の上限（既定値4、範囲1〜32）、`CONFIG_ZMK_DYNAMIC_MACRO_MAX_INSTRUCTIONS`はslotごとの命令数上限（既定値32、範囲1〜32）です。`CONFIG_ZMK_DYNAMIC_MACRO_MINIMUM_INTERVAL_MS`はBluetoothを無効にしたときのevent間隔で、既定値は5 msです。

初期programはbehavior nodeの`bindings`、`wait-ms`、`tap-ms`で指定できます。record容量を変更する場合はmanagerの`record-max-length`を指定します。
