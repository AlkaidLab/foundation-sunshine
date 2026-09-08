namespace Sunshine.Ds5Sidecar;

internal sealed class ControllerStateSubmissionPolicy
{
    private uint _buttons;
    private bool _analogNeutral = true;

    internal bool ObserveInput(uint buttons, bool analogNeutral)
    {
        var stateBoundary = buttons != _buttons || analogNeutral != _analogNeutral;
        _buttons = buttons;
        _analogNeutral = analogNeutral;
        return stateBoundary;
    }
}
