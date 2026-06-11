// tests/test_dom_api.cpp
// DOM API coverage real-site JS relies on: classList, getElementById /
// getElementsByClassName / getElementsByTagName, tree traversal, has/remove
// Attribute, and the CSSOM style object — driven through the WebCall ABI.

#include <gtest/gtest.h>
#include "malibu/view/view.h"

using malibu::view::View;

TEST(DomApi, ClassListAddRemoveToggleContains) {
    View v;
    v.load_html("<body><button id='b'>x</button></body>", "https://x/");
    auto r = v.eval_js(
        "(function(){ var b = document.querySelector('#b');"
        " b.classList.add('a', 'b');"
        " b.classList.toggle('c');"          // add c
        " b.classList.toggle('a');"          // remove a
        " return b.className + '|' + b.classList.contains('b') + '|' + b.classList.contains('a'); })()");
    EXPECT_EQ(r, "\"b c|true|false\"");
}

TEST(DomApi, GetElementByIdAndCollections) {
    View v;
    v.load_html("<body><ul id='list'><li class='item'>a</li><li class='item'>b</li></ul></body>", "https://x/");
    EXPECT_EQ(v.eval_js("document.getElementById('list').tagName"), "\"UL\"");
    EXPECT_EQ(v.eval_js("document.getElementsByClassName('item').length"), "2");
    EXPECT_EQ(v.eval_js("document.getElementsByTagName('li').length"), "2");
    EXPECT_EQ(v.eval_js("document.getElementsByClassName('item')[1].textContent"), "\"b\"");
}

TEST(DomApi, Traversal) {
    View v;
    v.load_html("<body><div id='p'><span id='a'>a</span><span id='b'>b</span></div></body>", "https://x/");
    EXPECT_EQ(v.eval_js("document.querySelector('#a').parentNode.id"), "\"p\"");
    EXPECT_EQ(v.eval_js("document.querySelector('#p').firstElementChild.id"), "\"a\"");
    EXPECT_EQ(v.eval_js("document.querySelector('#p').lastElementChild.id"), "\"b\"");
    EXPECT_EQ(v.eval_js("document.querySelector('#p').children.length"), "2");
    EXPECT_EQ(v.eval_js("document.querySelector('#a').nextSibling.id"), "\"b\"");
    EXPECT_EQ(v.eval_js("document.querySelector('#p').childElementCount"), "2");
}

TEST(DomApi, HasAndRemoveAttribute) {
    View v;
    v.load_html("<body><button id='b' data-role='x'>x</button></body>", "https://x/");
    auto r = v.eval_js(
        "(function(){ var b = document.querySelector('#b');"
        " var had = b.hasAttribute('data-role');"
        " b.removeAttribute('data-role');"
        " return had + '/' + b.hasAttribute('data-role'); })()");
    EXPECT_EQ(r, "\"true/false\"");
}

TEST(DomApi, StyleSetGetProperty) {
    View v;
    v.load_html("<body><div id='d'></div></body>", "https://x/");
    auto r = v.eval_js(
        "(function(){ var d = document.querySelector('#d');"
        " d.style.setProperty('color', 'red');"
        " d.style.setProperty('display', 'none');"
        " var c = d.style.getPropertyValue('color');"
        " d.style.removeProperty('display');"
        " return c + '|' + d.getAttribute('style'); })()");
    EXPECT_EQ(r, "\"red|color: red;\"");
}

TEST(DomApi, ClassListDrivesRendering) {
    // classList → class attribute → CSS cascade → re-render: pixel turns green.
    View v;
    v.load_html(
        "<body style='margin:0'><style>"
        "#box { background-color:#ff0000; width:60px; height:60px; }"
        "#box.on { background-color:#00ff00; }"
        "</style><div id='box'></div></body>",
        "https://x/");
    auto before = v.render(80, 80);
    EXPECT_EQ(before.at(20, 20), (malibu::render::Color{255, 0, 0, 255}));

    v.eval_js("document.querySelector('#box').classList.add('on')");
    auto after = v.render(80, 80);
    EXPECT_EQ(after.at(20, 20), (malibu::render::Color{0, 255, 0, 255}));
}
