docsearch({
    apiKey: '6c42f30d9669d8e42f6fc92f44028596',
    indexName: 'docs-ray',
    appId: 'LBHF0PABBL',
    inputSelector: '#search-input',
    debug: false,
    algoliaOptions: {
        hitsPerPage: 10,
    },
    handleSelected: function (input, event, suggestion, datasetNumber, context) {
        if (context.selectionMethod === 'click') {
            input.setVal('');
            const windowReference = window.open(suggestion.url, "_self");
            windowReference.focus();
        }
    }
});

const searchInput = document.getElementById("search-input");
const docUrlRegex = /\/(\w+)\/(\w+)\/.*/g;
searchInput.addEventListener("keydown", function (e) {
    if (e.code === "Enter") {
        var searchForm = document.getElementsByClassName("bd-search")[0];

        const [[pathname, lang, tag]] = window.location.pathname.matchAll(docUrlRegex);
        const text = searchInput.value;
        searchForm.action = `https://docs.ray.io/${lang}/${tag}/search.html?q=${text}`;

        searchForm.submit();
    }
});
