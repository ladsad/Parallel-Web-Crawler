#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>
#include <mpi.h>
#include <libxml/HTMLparser.h>

// Define the maximum number of links each process can discover
#define MAX_LINKS_PER_PROCESS 2
#define MAX_PAGES_TO_CRAWL 4  // Specify the maximum number of pages to crawl

// Callback function to handle the response data
size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t totalSize = size * nmemb;
    char *buffer = (char *)userp;
    memcpy(buffer, contents, totalSize);
    return totalSize;
}

// Callback function to extract links from cleaned and normalized HTML
void ExtractLinksFromHTML(const char *htmlContent, char *discoveredLinks[], int *numLinks) {
    htmlDocPtr doc = htmlReadMemory(htmlContent, strlen(htmlContent), NULL, "UTF-8", HTML_PARSE_RECOVER | HTML_PARSE_NOWARNING | HTML_PARSE_NOERROR);
    if (doc == NULL) {
        fprintf(stderr, "Error parsing HTML content.\n");
        return;
    }

    xmlNodePtr cur = xmlDocGetRootElement(doc);
    if (cur == NULL) {
        fprintf(stderr, "Empty HTML document.\n");
        xmlFreeDoc(doc);
        return;
    }

    while (cur != NULL) {
        if (cur->type == XML_ELEMENT_NODE) {
            if (cur->name != NULL) {
                if (xmlStrcmp(cur->name, (const xmlChar *)"a") == 0) {
                    xmlChar *href = xmlGetProp(cur, (const xmlChar *)"href");
                    if (href != NULL && *numLinks < MAX_LINKS_PER_PROCESS) {
                        // Add the discovered link to the shared array
                        discoveredLinks[(*numLinks)++] = strdup((char *)href);
                    }
                }
            }
        }
        cur = cur->next;
    }

    xmlFreeDoc(doc);
}

int main(int argc, char *argv[]) {
    int rank, size;
    MPI_Init(&argc, &argv);
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    // Initialize libcurl within each process
    CURL *curl;
    CURLcode res;
    curl = curl_easy_init();

    if (curl) {
        // List of seed URLs
        const char *seed_urls[] = {
            "https://example.com",
            "https://phet-dev.colorado.edu/html/build-an-atom/0.0.0-3/simple-text-only-test-page.html",
            "https://www.york.ac.uk/teaching/cws/wws/webpage1.html"
        };

        // Shared list for discovered links
        char *discoveredLinks[MAX_LINKS_PER_PROCESS];
        int numLinks = 0;
        char *allLinks[MAX_LINKS_PER_PROCESS * size];

        // Crawled page count
        int pagesCrawled = 0;

        // Calculate the range of URLs for this process
        int urlsPerProcess = (sizeof(seed_urls) + size - 1) / size;

        // Dynamic URL assignment by the master process (rank 0)
        if (rank == 0) {
            int urlsAssigned = 0;
            for (int i = 0; i < size; i++) {
                // Determine the URL to assign to process i
                int urlIndex = urlsAssigned;
                if (urlIndex < sizeof(seed_urls)) {
                    const char *urlToAssign = seed_urls[urlIndex];

                    // Send the URL to process i
                    MPI_Send(urlToAssign, strlen(urlToAssign) + 1, MPI_CHAR, i, 0, MPI_COMM_WORLD);
                    urlsAssigned++;
                }
            }
        }

        // Wait for the master process to assign URLs
        MPI_Barrier(MPI_COMM_WORLD);

        // Continue crawling until the termination condition is met
        while (pagesCrawled < MAX_PAGES_TO_CRAWL) {
            // Receive the assigned URL from the master process
            char urlBuffer[4096]; 
            MPI_Recv(urlBuffer, sizeof(urlBuffer), MPI_CHAR, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

            // Check if the received URL is null or empty
            if (urlBuffer == NULL || strlen(urlBuffer) == 0) {
                fprintf(stderr, "Process %d: Received null or empty URL\n", rank);
                break;  
            }

            curl_easy_setopt(curl, CURLOPT_URL, urlBuffer);

            // Buffer to store the downloaded page
            char pageBuffer[4096];  // Adjust the buffer size as needed

            // Set the callback function for the response data
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, pageBuffer);

            // Perform the request
            res = curl_easy_perform(curl);

            if (res != CURLE_OK) {
                // Handle HTTP request errors
                fprintf(stderr, "Process %d: Failed to fetch URL %s: %s\n", rank, urlBuffer, curl_easy_strerror(res));
            } else {
                // Page content is now in 'pageBuffer'
                printf("Process %d: Fetched page content for URL %s:\n%s\n", rank, urlBuffer, pageBuffer);
                char cleanedHTML[4096];

                // Extract links from the cleaned and normalized HTML content
                ExtractLinksFromHTML(cleanedHTML, discoveredLinks, &numLinks);
            }

            // Synchronize discovered links among processes
            int totalLinks[MAX_LINKS_PER_PROCESS * size];
            MPI_Allgather(&numLinks, 1, MPI_INT, totalLinks, 1, MPI_INT, MPI_COMM_WORLD);
            int totalNumLinks = 0;
            for (int i = 0; i < size; i++) {
                totalNumLinks += totalLinks[i];
            }

            MPI_Allgatherv(discoveredLinks, numLinks, MPI_CHAR, allLinks, totalLinks, totalLinks, MPI_CHAR, MPI_COMM_WORLD);

            // Clean up the discovered links
            for (int i = 0; i < numLinks; i++) {
                free(discoveredLinks[i]);
            }

            // Reset the number of discovered links
            numLinks = 0;

            // Print discovered links
            /*if (rank == 0) {
                printf("Number of links:%i\nDiscovered Links:\n",totalNumLinks);
                for (int i = 0; i < totalNumLinks; i++) {
                    if (allLinks[i] != NULL) {
                        printf("%s\n", allLinks[i]);
                        free(allLinks[i]);
                    }
                }
            }*/

            // Increment the crawled page count
            pagesCrawled++;
        }

        // Finalize libcurl within each process
        curl_easy_cleanup(curl);
    } else {
        fprintf(stderr, "Curl initialization failed.\n");
    }

    // Finalize MPI
    MPI_Finalize();
    return 0;
}
