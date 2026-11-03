using System;
using System.Collections.Generic;
using System.Linq;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Automation;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Input;
using Windows.System;
using WinMouseFix.Settings.Resources;

namespace WinMouseFix.Settings.Controls;

public sealed partial class BindingEditor : UserControl
{
    public ComboBox MouseButton => ButtonChoice;
    public ComboBox Trigger => TriggerChoice;
    public ComboBox Action => ActionChoice;
    public Button IdentifyButton => Identify;
    public CheckBox[] Modifiers => [ShiftModifier,CtrlModifier,AltModifier,WinModifier];
    public List<uint> ShortcutKeys { get; } = [];
    public bool ShortcutRecording { get; private set; }
    public bool ShortcutVisible => ShortcutPanel.Visibility == Visibility.Visible;
    public bool HasError => ErrorBar.IsOpen;
    public event System.Action? RecordingChanged;
    public BindingEditor() {
        InitializeComponent();
        TriggerHeading.Text = UiText.Get("触发条件"); ActionHeading.Text = UiText.Get("执行动作");
        ButtonChoice.Header = UiText.Get("鼠标按键"); TriggerChoice.Header = UiText.Get("触发方式"); ActionChoice.Header = UiText.Get("动作类型");
        ModifiersHeading.Text = UiText.Get("组合修饰键（可选）"); KeysBox.Header = UiText.Get("快捷键");
        KeysBox.PlaceholderText = UiText.Get("点录制，再在此处按下组合键。"); RecordShortcut.Content = UiText.Get("录制快捷键");
        ActionNote.Text = UiText.Get("仅选择“发送快捷键”时显示快捷键录制框。");
        AutomationProperties.SetName(ButtonChoice,UiText.Get("鼠标按键")); AutomationProperties.SetName(ActionChoice,UiText.Get("动作类型"));
        AutomationProperties.SetName(TriggerChoice,UiText.Get("触发方式")); AutomationProperties.SetName(KeysBox,UiText.Get("快捷键"));
        KeysBox.AddHandler(KeyDownEvent,new KeyEventHandler(RecordKey),true); SetIdentificationState(false);
        ButtonChoice.SelectionChanged += (_,_) => ErrorBar.IsOpen = false; TriggerChoice.SelectionChanged += (_,_) => ErrorBar.IsOpen = false;
        foreach(var modifier in Modifiers) modifier.Click += (_,_) => ErrorBar.IsOpen = false;
    }
    public void SetIdentificationState(bool active) {
        Identify.Content = UiText.Get(active ? "取消识别" : "识别鼠标按键");
        IdentifyNote.Text = UiText.Get(active ? "请按下要识别的鼠标按键；按 Esc 或关闭窗口可取消。" : "识别最长 30 秒；按 Esc 或关闭窗口可取消，不执行绑定动作。");
    }
    public void SetTriggerNote(string trigger) => TriggerNote.Text = UiText.Get(trigger switch {
        "click" => "单击所选按键时执行动作。", "double-click" => "连续单击两次时执行动作。", "hold" => "按住所选按键达到设定时间后执行动作。",
        "drag" => "按住按键并移动鼠标时执行动作。", "wheel" => "按住按键并滚动滚轮时执行动作。", "wheel-up" => "按住按键并向上滚动滚轮时执行动作。", _ => "按住按键并向下滚动滚轮时执行动作。" });
    public void SetKeys(IEnumerable<uint> keys) { ShortcutKeys.Clear(); ShortcutKeys.AddRange(keys); ShowKeys(); }
    public void ShowError(string message) { ErrorBar.Message = message; ErrorBar.IsOpen = true; DispatcherQueue.TryEnqueue(() => { ErrorBar.UpdateLayout(); ErrorBar.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false }); }); }
    private void ActionChanged(object sender,SelectionChangedEventArgs args) {
        if (ShortcutPanel is null) return;
        bool shortcut = ActionChoice.SelectedItem is ComboBoxItem item && (string)item.Tag == "shortcut";
        ShortcutPanel.Visibility = shortcut ? Visibility.Visible : Visibility.Collapsed;
        ErrorBar.IsOpen = false;
        if (shortcut) DispatcherQueue.TryEnqueue(() => { ShortcutPanel.UpdateLayout(); KeysBox.StartBringIntoView(new BringIntoViewOptions { AnimationDesired = false }); });
        if (!shortcut) StopRecording();
    }
    private void RecordShortcutClicked(object sender,RoutedEventArgs args) {
        if (ShortcutRecording) { StopRecording(); return; }
        ShortcutRecording = true; KeysBox.Text = UiText.Get("请按组合键；Esc 取消。"); RecordShortcut.Content = UiText.Get("取消录制"); RecordingChanged?.Invoke(); KeysBox.Focus(FocusState.Programmatic);
    }
    public void StopRecording() { ShortcutRecording = false; ShowKeys(); RecordShortcut.Content = UiText.Get("录制快捷键"); RecordingChanged?.Invoke(); }
    private void ShowKeys() => KeysBox.Text = string.Join(" + ",ShortcutKeys.Select(KeyName));
    private static string KeyName(uint key) => key switch { 16 => "Shift", 17 => "Ctrl", 18 => "Alt", 91 => "Win", _ => key is >=48 and <=90 ? ((char)key).ToString() : ((VirtualKey)key).ToString() };
    private void RecordKey(object sender,KeyRoutedEventArgs args) {
        if (!ShortcutRecording) return; args.Handled = true;
        if (args.Key == VirtualKey.Escape) { StopRecording(); return; }
        uint key = (uint)args.Key; if (key is 16 or 17 or 18 or 91 or 92) return;
        ShortcutKeys.Clear();
        foreach(var modifier in new[]{VirtualKey.Control,VirtualKey.Shift,VirtualKey.Menu,VirtualKey.LeftWindows})
            if ((Microsoft.UI.Input.InputKeyboardSource.GetKeyStateForCurrentThread(modifier) & Windows.UI.Core.CoreVirtualKeyStates.Down) != 0) ShortcutKeys.Add((uint)modifier);
        if (!ShortcutKeys.Contains(key)) ShortcutKeys.Add(key); StopRecording();
    }
}
