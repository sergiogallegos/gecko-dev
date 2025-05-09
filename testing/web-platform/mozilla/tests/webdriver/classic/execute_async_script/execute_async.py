import pytest
from tests.support.asserts import assert_success
from tests.support.sync import Poll
from webdriver.error import NoSuchAlertException


def execute_async_script(session, script, args=None):
    if args is None:
        args = []
    body = {"script": script, "args": args}

    return session.transport.send(
        "POST", "/session/{session_id}/execute/async".format(**vars(session)), body
    )


@pytest.mark.parametrize("dialog_type", ["alert", "confirm", "prompt"])
def test_no_abort_by_user_prompt_in_other_tab(session, inline, dialog_type):
    original_handle = session.window_handle
    original_handles = session.handles

    session.url = inline(
        f"""
      <a onclick="window.open();">open window</a>
      <script>
        window.addEventListener("message", function (event) {{
          {dialog_type}("foo");
        }});
      </script>
    """
    )

    session.find.css("a", all=False).click()
    wait = Poll(session, timeout=5, message="No new window has been opened")
    new_handles = wait.until(lambda s: set(s.handles) - set(original_handles))
    assert len(new_handles) == 1

    session.window_handle = new_handles.pop()

    response = execute_async_script(
        session,
        """
        const resolve = arguments[0];

        // Trigger opening a user prompt in the other window.
        window.opener.postMessage("foo", "*");

        // Delay resolving the Promise to ensure a user prompt has been opened.
        setTimeout(() => resolve(42), 500);
        """,
    )

    assert_success(response, 42)

    session.window.close()

    session.window_handle = original_handle

    # Opening the alert in a different window is async here and can cause
    # delays in slow builds like CCOV or TSAN.
    wait = Poll(
        session,
        timeout=15,
        ignored_exceptions=NoSuchAlertException,
        message="No user prompt with text 'foo' detected",
    )
    wait.until(lambda s: s.alert.text == "foo")

    session.alert.accept()
